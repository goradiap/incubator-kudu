// Copyright (c) 2013, Cloudera,inc.

#include <boost/bind.hpp>
#include <boost/thread/locks.hpp>

#include "client/batcher.h"
#include "client/client.h"
#include "client/error_collector.h"
#include "client/meta_cache.h"
#include "common/row.h"
#include "common/wire_protocol.h"
#include "gutil/casts.h"
#include "gutil/stl_util.h"
#include "gutil/strings/substitute.h"
#include "master/master.h" // TODO: remove this include - just needed for default port
#include "master/master.proxy.h"
#include "rpc/messenger.h"
#include "util/async_util.h"
#include "util/countdown_latch.h"
#include "util/net/dns_resolver.h"
#include "util/net/net_util.h"
#include "util/status.h"

#include <algorithm>
#include <tr1/memory>
#include <vector>

using std::string;
using std::tr1::shared_ptr;
using std::vector;
using strings::Substitute;
using kudu::master::MasterServiceProxy;
using kudu::master::AlterTableRequestPB;
using kudu::master::AlterTableResponsePB;
using kudu::master::CreateTableRequestPB;
using kudu::master::CreateTableResponsePB;
using kudu::master::DeleteTableRequestPB;
using kudu::master::DeleteTableResponsePB;
using kudu::master::GetTableSchemaRequestPB;
using kudu::master::GetTableSchemaResponsePB;
using kudu::master::GetTableLocationsRequestPB;
using kudu::master::GetTableLocationsResponsePB;
using kudu::master::IsAlterTableDoneRequestPB;
using kudu::master::IsAlterTableDoneResponsePB;
using kudu::master::IsCreateTableDoneRequestPB;
using kudu::master::IsCreateTableDoneResponsePB;
using kudu::master::MasterServiceProxy;
using kudu::tserver::ColumnRangePredicatePB;
using kudu::tserver::NewScanRequestPB;
using kudu::tserver::ScanRequestPB;
using kudu::tserver::ScanResponsePB;
using kudu::tserver::TabletServerServiceProxy;
using kudu::rpc::MessengerBuilder;
using kudu::rpc::RpcController;

MAKE_ENUM_LIMITS(kudu::client::KuduSession::FlushMode,
                 kudu::client::KuduSession::AUTO_FLUSH_SYNC,
                 kudu::client::KuduSession::MANUAL_FLUSH);

namespace kudu {
namespace client {

using internal::Batcher;
using internal::ErrorCollector;

// Retry helper, takes a function like: Status funcName(const MonoTime& deadline, bool *retry, ...)
// The function should set the retry flag (default true) if the function should
// be retried again. On retry == false the return status of the function will be
// returned to the caller, otherwise a Status::Timeout() will be returned.
// If the deadline is already expired, no attempt will be made.
static Status RetryFunc(const MonoTime& deadline,
                        const string& retry_msg,
                        const string& timeout_msg,
                        const boost::function<Status(const MonoTime&, bool*)>& func) {
  MonoTime now = MonoTime::Now(MonoTime::FINE);
  if (!now.ComesBefore(deadline)) {
    return Status::TimedOut(timeout_msg);
  }

  int64_t wait_time = 1000;
  while (1) {
    MonoTime stime = now;
    bool retry = true;
    Status s = func(deadline, &retry);
    if (!retry) {
      return s;
    }

    now = MonoTime::Now(MonoTime::FINE);
    if (!now.ComesBefore(deadline)) {
      break;
    }

    VLOG(1) << retry_msg << " status=" << s.ToString();
    int64_t timeout_usec = deadline.GetDeltaSince(now).ToNanoseconds() -
                           now.GetDeltaSince(stime).ToNanoseconds();
    if (timeout_usec > 0) {
      wait_time = std::min(wait_time * 5 / 4, timeout_usec);
      usleep(wait_time);
      now = MonoTime::Now(MonoTime::FINE);
    }
  }

  return Status::TimedOut(timeout_msg);
}

KuduClientOptions::KuduClientOptions()
  : default_admin_operation_timeout(MonoDelta::FromMilliseconds(5 * 1000)) {
}

Status KuduClient::Create(const KuduClientOptions& options,
                          std::tr1::shared_ptr<KuduClient>* client) {
  shared_ptr<KuduClient> c(new KuduClient(options));
  RETURN_NOT_OK(c->Init());
  client->swap(c);
  return Status::OK();
}

KuduClient::KuduClient(const KuduClientOptions& options)
  : initted_(false),
    options_(options) {
}

Status KuduClient::Init() {
  // Init messenger.
  if (options_.messenger) {
    messenger_ = options_.messenger;
  } else {
    MessengerBuilder builder("client");
    RETURN_NOT_OK(builder.Build(&messenger_));
  }

  // Init proxy.
  vector<Sockaddr> addrs;
  RETURN_NOT_OK(ParseAddressList(options_.master_server_addr,
                                 master::Master::kDefaultPort, &addrs));
  if (addrs.empty()) {
    return Status::InvalidArgument("No master address specified");
  }
  if (addrs.size() > 1) {
    LOG(WARNING) << "Specified master server address '" << options_.master_server_addr << "' "
                 << "resolved to multiple IPs. Using " << addrs[0].ToString();
  }
  master_proxy_.reset(new MasterServiceProxy(messenger_, addrs[0]));

  meta_cache_.reset(new MetaCache(this));
  dns_resolver_.reset(new DnsResolver());

  initted_ = true;

  return Status::OK();
}

Status KuduClient::CreateTable(const std::string& table_name,
                               const Schema& schema) {
  return CreateTable(table_name, schema, CreateTableOptions());
}

Status KuduClient::CreateTable(const std::string& table_name,
                               const Schema& schema,
                               const CreateTableOptions& opts) {
  CreateTableRequestPB req;
  CreateTableResponsePB resp;
  RpcController rpc;

  MonoTime deadline = MonoTime::Now(MonoTime::FINE);
  deadline.AddDelta(MonoDelta::FromMilliseconds(15 * 1000));

  req.set_name(table_name);
  rpc.set_timeout(options_.default_admin_operation_timeout);
  RETURN_NOT_OK_PREPEND(SchemaToPB(schema, req.mutable_schema()),
                        "Invalid schema");

  BOOST_FOREACH(const std::string& key, opts.split_keys_) {
    req.add_pre_split_keys(key);
  }

  RETURN_NOT_OK(master_proxy_->CreateTable(req, &resp, &rpc));
  if (resp.has_error()) {
    // TODO: if already exist and in progress spin
    return StatusFromPB(resp.error().status());
  }

  if (opts.wait_assignment_) {
    RETURN_NOT_OK(RetryFunc(deadline,
          "Waiting on Create Table to be completed",
          "Timeout out waiting for Table Creation",
          boost::bind(&KuduClient::IsCreateTableInProgress, this, table_name, _1, _2)));
  }

  return Status::OK();
}

Status KuduClient::IsCreateTableInProgress(const std::string& table_name,
                                           bool *create_in_progress) {
  MonoTime deadline = MonoTime::Now(MonoTime::FINE);
  deadline.AddDelta(options_.default_admin_operation_timeout);
  return IsCreateTableInProgress(table_name, deadline, create_in_progress);
}

Status KuduClient::IsCreateTableInProgress(const std::string& table_name,
                                           const MonoTime& deadline,
                                           bool *create_in_progress) {
  IsCreateTableDoneRequestPB req;
  IsCreateTableDoneResponsePB resp;
  RpcController rpc;

  req.mutable_table()->set_table_name(table_name);
  rpc.set_timeout(deadline.GetDeltaSince(MonoTime::Now(MonoTime::FINE)));
  RETURN_NOT_OK(master_proxy_->IsCreateTableDone(req, &resp, &rpc));
  if (resp.has_error()) {
    return StatusFromPB(resp.error().status());
  }

  *create_in_progress = !resp.done();
  return Status::OK();
}

Status KuduClient::DeleteTable(const std::string& table_name) {
  DeleteTableRequestPB req;
  DeleteTableResponsePB resp;
  RpcController rpc;

  req.mutable_table()->set_table_name(table_name);
  rpc.set_timeout(options_.default_admin_operation_timeout);
  RETURN_NOT_OK(master_proxy_->DeleteTable(req, &resp, &rpc));
  if (resp.has_error()) {
    return StatusFromPB(resp.error().status());
  }

  return Status::OK();
}

Status KuduClient::AlterTable(const std::string& table_name,
                              const AlterTableBuilder& alter) {
  AlterTableRequestPB req;
  AlterTableResponsePB resp;
  RpcController rpc;

  if (!alter.has_changes()) {
    return Status::InvalidArgument("No alter steps provided");
  }

  MonoTime deadline = MonoTime::Now(MonoTime::FINE);
  deadline.AddDelta(MonoDelta::FromMilliseconds(60 * 1000));

  req.CopyFrom(*alter.alter_steps_);
  req.mutable_table()->set_table_name(table_name);
  rpc.set_timeout(options_.default_admin_operation_timeout);
  RETURN_NOT_OK(master_proxy_->AlterTable(req, &resp, &rpc));
  if (resp.has_error()) {
    return StatusFromPB(resp.error().status());
  }

  string alter_name = req.has_new_table_name() ? req.new_table_name() : table_name;
  RETURN_NOT_OK(RetryFunc(deadline,
        "Waiting on Alter Table to be completed",
        "Timeout out waiting for AlterTable",
        boost::bind(&KuduClient::IsAlterTableInProgress, this, alter_name, _1, _2)));

  return Status::OK();
}

Status KuduClient::IsAlterTableInProgress(const std::string& table_name,
                                          bool *alter_in_progress) {
  MonoTime deadline = MonoTime::Now(MonoTime::FINE);
  deadline.AddDelta(options_.default_admin_operation_timeout);
  return IsAlterTableInProgress(table_name, deadline, alter_in_progress);
}

Status KuduClient::IsAlterTableInProgress(const std::string& table_name,
                                          const MonoTime& deadline,
                                          bool *alter_in_progress) {
  IsAlterTableDoneRequestPB req;
  IsAlterTableDoneResponsePB resp;
  RpcController rpc;

  req.mutable_table()->set_table_name(table_name);
  rpc.set_timeout(deadline.GetDeltaSince(MonoTime::Now(MonoTime::FINE)));
  RETURN_NOT_OK(master_proxy_->IsAlterTableDone(req, &resp, &rpc));
  if (resp.has_error()) {
    return StatusFromPB(resp.error().status());
  }

  *alter_in_progress = !resp.done();
  return Status::OK();
}

Status KuduClient::GetTableSchema(const std::string& table_name,
                                  Schema *schema) {
  GetTableSchemaRequestPB req;
  GetTableSchemaResponsePB resp;
  RpcController rpc;

  req.mutable_table()->set_table_name(table_name);
  rpc.set_timeout(options_.default_admin_operation_timeout);
  RETURN_NOT_OK(master_proxy_->GetTableSchema(req, &resp, &rpc));
  if (resp.has_error()) {
    return StatusFromPB(resp.error().status());
  }

  Schema server_schema;
  RETURN_NOT_OK(SchemaFromPB(resp.schema(), &server_schema));

  // Remove the server IDs from the schema
  schema->Reset(server_schema.columns(), server_schema.num_key_columns());
  return Status::OK();
}

Status KuduClient::OpenTable(const std::string& table_name,
                             scoped_refptr<KuduTable>* table) {
  CHECK(initted_) << "Must Init()";

  Schema schema;
  RETURN_NOT_OK(GetTableSchema(table_name, &schema));

  // In the future, probably will look up the table in some map to reuse KuduTable
  // instances.
  scoped_refptr<KuduTable> ret(new KuduTable(shared_from_this(), table_name, schema));
  RETURN_NOT_OK(ret->Open());
  table->swap(ret);

  return Status::OK();
}

shared_ptr<KuduSession> KuduClient::NewSession() {
  shared_ptr<KuduSession> ret(new KuduSession(shared_from_this()));
  ret->Init();
  return ret;
}

Status KuduClient::GetTabletProxy(const std::string& tablet_id,
                                  shared_ptr<TabletServerServiceProxy>* proxy) {
  // TODO: write a proper async version of this for async client.
  shared_ptr<RemoteTablet> remote_tablet;
  meta_cache_->LookupTabletByID(tablet_id, &remote_tablet);

  Synchronizer s;
  remote_tablet->Refresh(this, s.callback(), false);
  RETURN_NOT_OK(s.Wait());

  RemoteTabletServer* ts = remote_tablet->replica_tserver(0);
  if (ts == NULL) {
    return Status::NotFound(Substitute("No replicas for tablet $0", tablet_id));
  }

  s.Reset();
  ts->RefreshProxy(this, s.callback(), false);
  RETURN_NOT_OK(s.Wait());

  *proxy = ts->proxy();
  return Status::OK();
}

////////////////////////////////////////////////////////////
// CreateTableOptions
////////////////////////////////////////////////////////////

CreateTableOptions::CreateTableOptions()
  : wait_assignment_(true) {
}

CreateTableOptions::~CreateTableOptions() {
}

CreateTableOptions& CreateTableOptions::WithSplitKeys(
    const std::vector<std::string>& keys) {
  split_keys_ = keys;
  return *this;
}

CreateTableOptions& CreateTableOptions::WaitAssignment(bool wait_assignment) {
  wait_assignment_ = wait_assignment;
  return *this;
}

////////////////////////////////////////////////////////////
// KuduTable
////////////////////////////////////////////////////////////

KuduTable::KuduTable(const std::tr1::shared_ptr<KuduClient>& client,
                     const std::string& name,
                     const Schema& schema)
  : client_(client),
    name_(name),
    schema_(schema) {
}

KuduTable::~KuduTable() {
}

Status KuduTable::Open() {
  // TODO: fetch the schema from the master here once catalog is available.
  GetTableLocationsRequestPB req;
  GetTableLocationsResponsePB resp;

  req.mutable_table()->set_table_name(name_);
  do {
    rpc::RpcController rpc;
    RETURN_NOT_OK(client_->master_proxy()->GetTableLocations(req, &resp, &rpc));
    if (resp.has_error()) {
      return StatusFromPB(resp.error().status());
    }

    if (resp.tablet_locations_size() > 0)
      break;

    /* TODO: Add a timeout or number of retries */
    usleep(100000);
  } while (1);

  // TODO: we can use the info inside the resp...
  // TODO: some code relies on table->name() as tablet id
  DCHECK_EQ(1, resp.tablet_locations_size()) << "Only one tablet supported by the client";
  tablet_id_ = resp.tablet_locations(0).tablet_id();
  VLOG(1) << "Open Table " << name_ << ", found tablet=" << tablet_id_;
  return Status::OK();
}

std::tr1::shared_ptr<tserver::TabletServerServiceProxy> KuduTable::proxy() {
  boost::lock_guard<simple_spinlock> l(lock_);
  if (proxy_) {
    return proxy_;
  }
  // CHECK is wrong here, but this whole method is going away sooner or later...
  CHECK_OK(client_->GetTabletProxy(tablet_id_, &proxy_));
  return proxy_;
}


gscoped_ptr<Insert> KuduTable::NewInsert() {
  return gscoped_ptr<Insert>(new Insert(this));
}

////////////////////////////////////////////////////////////
// Error
////////////////////////////////////////////////////////////
Error::Error(gscoped_ptr<Insert> failed_op,
             const Status& status) :
  failed_op_(failed_op.Pass()),
  status_(status) {
}

Error::~Error() {
}

////////////////////////////////////////////////////////////
// KuduSession
////////////////////////////////////////////////////////////

KuduSession::KuduSession(const std::tr1::shared_ptr<KuduClient>& client)
  : client_(client),
    error_collector_(new ErrorCollector()),
    flush_mode_(AUTO_FLUSH_SYNC),
    timeout_ms_(0) {
}

KuduSession::~KuduSession() {
  if (batcher_->HasPendingOperations()) {
    LOG(WARNING) << "Closing Session with pending operations.";
  }
  batcher_->Abort();
}

void KuduSession::Init() {
  boost::lock_guard<simple_spinlock> l(lock_);
  CHECK(!batcher_);
  NewBatcher(NULL);
}

void KuduSession::NewBatcher(scoped_refptr<Batcher>* old_batcher) {
  DCHECK(lock_.is_locked());

  scoped_refptr<Batcher> batcher(
    new Batcher(client_.get(), error_collector_.get(), shared_from_this()));
  batcher->SetTimeoutMillis(timeout_ms_);
  batcher.swap(batcher_);

  if (old_batcher) {
    old_batcher->swap(batcher);
  }
}

Status KuduSession::SetFlushMode(FlushMode m) {
  if (batcher_->HasPendingOperations()) {
    // TODO: there may be a more reasonable behavior here.
    return Status::IllegalState("Cannot change flush mode when writes are buffered");
  }
  if (!tight_enum_test<FlushMode>(m)) {
    // Be paranoid in client code.
    return Status::InvalidArgument("Bad flush mode");
  }

  flush_mode_ = m;
  return Status::OK();
}

void KuduSession::SetTimeoutMillis(int millis) {
  CHECK_GE(millis, 0);
  timeout_ms_ = millis;
  batcher_->SetTimeoutMillis(millis);
}

Status KuduSession::Apply(gscoped_ptr<Insert>* insert_scoped) {
  Insert* insert = insert_scoped->get();
  if (!insert->row().IsKeySet()) {
    return Status::IllegalState("Key not specified", insert->ToString());
  }

  batcher_->Add(insert_scoped->Pass());

  if (flush_mode_ == AUTO_FLUSH_SYNC) {
    RETURN_NOT_OK(Flush());
  }

  return Status::OK();
}

Status KuduSession::Flush() {
  Synchronizer s;
  FlushAsync(s.callback());
  return s.Wait();
}

void KuduSession::FlushAsync(const StatusCallback& user_callback) {
  CHECK_EQ(flush_mode_, MANUAL_FLUSH) << "TODO: handle other flush modes";

  // Swap in a new batcher to start building the next batch.
  // Save off the old batcher.
  scoped_refptr<Batcher> old_batcher;
  {
    boost::lock_guard<simple_spinlock> l(lock_);
    NewBatcher(&old_batcher);
    InsertOrDie(&flushed_batchers_, old_batcher.get());
  }

  // Send off any buffered data. Important to do this outside of the lock
  // since the callback may itself try to take the lock, in the case that
  // the batch fails "inline" on the same thread.
  old_batcher->FlushAsync(user_callback);
}

void KuduSession::FlushFinished(Batcher* batcher) {
  boost::lock_guard<simple_spinlock> l(lock_);
  CHECK_EQ(flushed_batchers_.erase(batcher), 1);
}

bool KuduSession::HasPendingOperations() const {
  boost::lock_guard<simple_spinlock> l(lock_);
  if (batcher_->HasPendingOperations()) {
    return true;
  }
  BOOST_FOREACH(Batcher* b, flushed_batchers_) {
    if (b->HasPendingOperations()) {
      return true;
    }
  }
  return false;
}

int KuduSession::CountBufferedOperations() const {
  boost::lock_guard<simple_spinlock> l(lock_);
  CHECK_EQ(flush_mode_, MANUAL_FLUSH);

  return batcher_->CountBufferedOperations();
}

int KuduSession::CountPendingErrors() const {
  return error_collector_->CountErrors();
}

void KuduSession::GetPendingErrors(std::vector<Error*>* errors, bool* overflowed) {
  error_collector_->GetErrors(errors, overflowed);
}

////////////////////////////////////////////////////////////
// Mutation classes (Insert/Update/Delete)
////////////////////////////////////////////////////////////

Insert::Insert(KuduTable* table)
  : table_(table),
    row_(&table->schema()) {
}

Insert::~Insert() {}

////////////////////////////////////////////////////////////
// AlterTable
////////////////////////////////////////////////////////////
AlterTableBuilder::AlterTableBuilder()
  : alter_steps_(new AlterTableRequestPB) {
}

AlterTableBuilder::~AlterTableBuilder() {
  delete alter_steps_;
}

void AlterTableBuilder::Reset() {
  alter_steps_->clear_alter_schema_steps();
}

bool AlterTableBuilder::has_changes() const {
  return alter_steps_->has_new_table_name() ||
         alter_steps_->alter_schema_steps_size() > 0;
}

Status AlterTableBuilder::RenameTable(const string& new_name) {
  alter_steps_->set_new_table_name(new_name);
  return Status::OK();
}

Status AlterTableBuilder::AddColumn(const std::string& name,
                                    DataType type,
                                    const void *default_value,
                                    ColumnStorageAttributes attributes) {
  if (default_value == NULL) {
    return Status::InvalidArgument("A new column must have a default value",
                                   "Use AddNullableColumn() to add a NULLABLE column");
  }

  AlterTableRequestPB::Step* step = alter_steps_->add_alter_schema_steps();
  step->set_type(AlterTableRequestPB::ADD_COLUMN);
  ColumnSchemaToPB(ColumnSchema(name, type, false, default_value, default_value, attributes),
                                step->mutable_add_column()->mutable_schema());
  return Status::OK();
}

Status AlterTableBuilder::AddNullableColumn(const std::string& name,
                                            DataType type,
                                            ColumnStorageAttributes attributes) {
  AlterTableRequestPB::Step* step = alter_steps_->add_alter_schema_steps();
  step->set_type(AlterTableRequestPB::ADD_COLUMN);
  ColumnSchemaToPB(ColumnSchema(name, type, true, NULL, NULL, attributes),
                                step->mutable_add_column()->mutable_schema());
  return Status::OK();
}

Status AlterTableBuilder::DropColumn(const std::string& name) {
  AlterTableRequestPB::Step* step = alter_steps_->add_alter_schema_steps();
  step->set_type(AlterTableRequestPB::DROP_COLUMN);
  step->mutable_drop_column()->set_name(name);
  return Status::OK();
}

Status AlterTableBuilder::RenameColumn(const std::string& old_name,
                                       const std::string& new_name) {
  AlterTableRequestPB::Step* step = alter_steps_->add_alter_schema_steps();
  step->set_type(AlterTableRequestPB::RENAME_COLUMN);
  step->mutable_rename_column()->set_old_name(old_name);
  step->mutable_rename_column()->set_new_name(new_name);
  return Status::OK();
}

////////////////////////////////////////////////////////////
// KuduScanner
////////////////////////////////////////////////////////////

KuduScanner::KuduScanner(KuduTable* table)
  : open_(false),
    data_in_open_(false),
    table_(DCHECK_NOTNULL(table)) {
}

KuduScanner::~KuduScanner() {
  Close();
}

Status KuduScanner::SetProjection(const Schema& projection) {
  CHECK(!open_) << "Scanner already open";
  projection_ = projection;
  NewScanRequestPB* scan = next_req_.mutable_new_scan_request();
  RETURN_NOT_OK(SchemaToColumnPBs(projection, scan->mutable_projected_columns()));
  return Status::OK();
}

Status KuduScanner::SetBatchSizeBytes(uint32_t batch_size) {
  next_req_.set_batch_size_bytes(batch_size);
  return Status::OK();
}

Status KuduScanner::AddConjunctPredicate(const ColumnRangePredicatePB& pb) {
  CHECK(!open_) << "Scanner already open";
  NewScanRequestPB* scan = next_req_.mutable_new_scan_request();
  scan->add_range_predicates()->CopyFrom(pb);
  return Status::OK();
}

Status KuduScanner::Open() {
  CHECK(!open_) << "Scanner already open";

  // TODO: Replace with a request to locations by start/end key
  next_req_.mutable_new_scan_request()->set_tablet_id(table_->tablet_id_);

  controller_.Reset();
  // TODO: make configurable through API
  const int kOpenTimeoutMs = 5000;
  controller_.set_timeout(MonoDelta::FromMilliseconds(kOpenTimeoutMs));

  RETURN_NOT_OK(table_->proxy()->Scan(next_req_, &last_response_, &controller_));
  RETURN_NOT_OK(CheckForErrors());
  data_in_open_ = last_response_.has_data();

  next_req_.clear_new_scan_request();
  if (last_response_.has_more_results()) {
    next_req_.set_scanner_id(last_response_.scanner_id());
    VLOG(1) << "Started scanner " << last_response_.scanner_id();
  } else {
    VLOG(1) << "Scanner matched no rows, no scanner ID assigned.";
  }

  open_ = true;
  return Status::OK();
}

namespace {
// Callback for the RPC sent by Close().
// We can't use the KuduScanner response and RPC controller members for this
// call, because the scanner object may be destructed while the call is still
// being processed.
struct CloseCallback {
  RpcController controller;
  ScanResponsePB response;
  string scanner_id;
  void Callback() {
    if (!controller.status().ok()) {
      LOG(WARNING) << "Couldn't close scanner " << scanner_id << ": "
                   << controller.status().ToString();
    }
    delete this;
  }
};
} // anonymous namespace

void KuduScanner::Close() {
  if (!open_) return;

  if (next_req_.scanner_id().empty()) {
    // In the case that the scan matched no rows, and this was determined
    // in the Open() call, then we won't have been assigned a scanner ID
    // at all. So, no need to close on the server side.
    open_ = false;
    return;
  }

  CloseCallback* closer = new CloseCallback;
  closer->scanner_id = next_req_.scanner_id();
  next_req_.set_batch_size_bytes(0);
  next_req_.set_close_scanner(true);
  closer->controller.set_timeout(MonoDelta::FromMilliseconds(5000));
  table_->proxy()->ScanAsync(next_req_, &closer->response, &closer->controller,
                            boost::bind(&CloseCallback::Callback, closer));
  next_req_.Clear();
  open_ = false;
}

Status KuduScanner::CheckForErrors() {
  if (PREDICT_TRUE(!last_response_.has_error())) {
    return Status::OK();
  }

  return StatusFromPB(last_response_.error().status());
}

bool KuduScanner::HasMoreRows() const {
  CHECK(open_);
  return data_in_open_ || last_response_.has_more_results();
}

Status KuduScanner::NextBatch(std::vector<const uint8_t*>* rows) {
  // TODO: do some double-buffering here -- when we return this batch
  // we should already have fired off the RPC for the next batch, but
  // need to do some swapping of the response objects around to avoid
  // stomping on the memory the user is looking at.
  CHECK(open_);
  if (!data_in_open_) {
    controller_.Reset();
    rows->clear();
    RETURN_NOT_OK(table_->proxy_->Scan(next_req_, &last_response_, &controller_));
    RETURN_NOT_OK(CheckForErrors());
  } else {
    data_in_open_ = false;
  }

  RETURN_NOT_OK(ExtractRowsFromRowBlockPB(projection_, last_response_.mutable_data(), rows));
  return Status::OK();
}


} // namespace client
} // namespace kudu
