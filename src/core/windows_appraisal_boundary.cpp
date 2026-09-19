#include "sketch/windows_appraisal_boundary.hpp"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <array>
#include <atomic>
#include <set>
#include <stdexcept>
#include <thread>
#include <utility>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <bcrypt.h>
#endif

namespace sketch {
namespace {
using Json = nlohmann::json;
void require(bool value) { if (!value) throw std::invalid_argument("invalid appraisal worker protocol"); }
bool valid_id(const std::string &s) {
  return !s.empty() && s.size() <= 128 && std::all_of(s.begin(),s.end(),[](unsigned char c) {
    return (c>='a' && c<='z') || (c>='A' && c<='Z') || (c>='0' && c<='9') || c=='.' || c=='_' || c=='-';
  });
}
std::string frame(std::string body) {
  std::string result(4,'\0');
  for (unsigned i=0;i<4;++i) result[i]=static_cast<char>((body.size()>>(i*8)) & 255);
  result += body;
  return result;
}
AppraisalWorkerReply decode(const std::string &body) {
  std::vector<std::set<std::string>> keys;
  auto callback=[&](int depth, Json::parse_event_t event, Json &value) {
    require(depth<=16);
    if(event==Json::parse_event_t::object_start) keys.emplace_back();
    if(event==Json::parse_event_t::key) require(keys.back().insert(value.get<std::string>()).second);
    if(event==Json::parse_event_t::object_end) keys.pop_back();
    return true;
  };
  const auto j=Json::parse(body,callback);
  require(j.is_object() && j.size()==5 && j.contains("version") && j.contains("kind") &&
    j.contains("request_id") && j.contains("capabilities") && j.contains("prepared_payload"));
  require(j.at("version").is_number_unsigned() && j.at("version")==1 && j.at("kind").is_string() &&
    j.at("request_id").is_string() && j.at("capabilities").is_array() && j.at("prepared_payload").is_string());
  AppraisalWorkerReply reply;
  reply.request_id=j.at("request_id").get<std::string>();
  require(valid_id(reply.request_id));
  const auto kind=j.at("kind").get<std::string>();
  if(kind=="capabilities") reply.kind=AppraisalReplyKind::capabilities;
  else if(kind=="prepared") reply.kind=AppraisalReplyKind::prepared;
  else if(kind=="offline") reply.kind=AppraisalReplyKind::offline;
  else if(kind=="unavailable") reply.kind=AppraisalReplyKind::unavailable;
  else if(kind=="unsupported") reply.kind=AppraisalReplyKind::unsupported;
  else if(kind=="ambiguous") reply.kind=AppraisalReplyKind::ambiguous;
  else if(kind=="worker_failure") reply.kind=AppraisalReplyKind::worker_failure;
  else if(kind=="uncertain_response") reply.kind=AppraisalReplyKind::uncertain_response;
  else require(false);
  require(j.at("capabilities").size()<=64);
  for(const auto &capability:j.at("capabilities"))
    reply.capabilities.push_back(parse_appraisal_mapping_json(capability.dump()));
  reply.prepared_payload=j.at("prepared_payload").get<std::string>();
  require(reply.kind==AppraisalReplyKind::capabilities || reply.capabilities.empty());
  require(reply.kind==AppraisalReplyKind::prepared || reply.prepared_payload.empty());
  return reply;
}
}
struct WindowsAppraisalBoundary::State {
  std::atomic<bool> abandoned{}, finished{};
  std::atomic<std::uint32_t> pid{};
  std::atomic<int> failure{}; // 1 transport/process failure, 2 malformed framing/protocol
  std::atomic<std::shared_ptr<std::string>> outbound;
  std::atomic<std::shared_ptr<AppraisalWorkerReply>> inbound;
  std::uint32_t limit{};
};
WindowsAppraisalBoundary::WindowsAppraisalBoundary(WindowsAppraisalOptions options)
    :state_(std::make_shared<State>()) {
  require(options.max_frame_bytes>=256 && options.max_frame_bytes<=4*1024*1024);
  require(options.executable.is_absolute() && options.executable.native().find(std::filesystem::path::value_type{})==std::filesystem::path::string_type::npos);
#ifdef _WIN32
  const auto root=options.executable.root_name().wstring();
  require(root.size()==2 && root[1]==L':');
#endif
  state_->limit=options.max_frame_bytes;
  std::thread(&WindowsAppraisalBoundary::supervise,state_,std::move(options)).detach();
}
WindowsAppraisalBoundary::~WindowsAppraisalBoundary() { abandon(); }
void WindowsAppraisalBoundary::send(std::string message) {
  require(!state_->abandoned.load() && message.size()<=state_->limit);
  auto framed=std::make_shared<std::string>(frame(std::move(message)));
  std::shared_ptr<std::string> empty;
  require(state_->outbound.compare_exchange_strong(empty,std::move(framed)));
}
void WindowsAppraisalBoundary::negotiate(const std::string &id) {
  require(phase_==0 && valid_id(id));
  request_id_=id;
  send(Json{{"version",1},{"kind","negotiate"},{"request_id",id}}.dump());
  phase_=1;
}
void WindowsAppraisalBoundary::prepare(const std::string &id,const std::string &payload) {
  require(phase_==2 && id==request_id_ && payload.size()<=state_->limit);
  send(Json{{"version",1},{"kind","prepare"},{"request_id",id},{"payload",payload}}.dump());
  phase_=3;
}
std::optional<AppraisalWorkerReply> WindowsAppraisalBoundary::poll() {
  if(state_->abandoned.load()) return {};
  const auto failure=state_->failure.load();
  if(failure) return AppraisalWorkerReply{failure==2 ? AppraisalReplyKind::uncertain_response : AppraisalReplyKind::worker_failure,request_id_,{}, {}};
  auto reply=state_->inbound.exchange({});
  if(!reply) return {};
  if(phase_==1 && reply->kind==AppraisalReplyKind::capabilities) phase_=2;
  return std::move(*reply);
}
void WindowsAppraisalBoundary::abandon() noexcept { state_->abandoned.store(true); }
std::uint32_t WindowsAppraisalBoundary::process_id() const noexcept { return state_->pid.load(); }
bool WindowsAppraisalBoundary::supervisor_finished() const noexcept { return state_->finished.load(); }

#ifdef _WIN32
namespace {
class Handle {
public:
  HANDLE value{};
  explicit Handle(HANDLE h=nullptr):value(h) {}
  ~Handle() { if(value && value!=INVALID_HANDLE_VALUE) CloseHandle(value); }
  Handle(const Handle &)=delete;
  Handle &operator=(const Handle &)=delete;
  bool valid() const { return value && value!=INVALID_HANDLE_VALUE; }
};
class Attributes {
  std::vector<unsigned char> storage_;
public:
  LPPROC_THREAD_ATTRIBUTE_LIST value{};
  Attributes() {
    SIZE_T size{}; InitializeProcThreadAttributeList(nullptr,1,0,&size);
    storage_.resize(size); value=reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage_.data());
    if(!InitializeProcThreadAttributeList(value,1,0,&size)) { value=nullptr; throw std::runtime_error("attribute initialization"); }
  }
  ~Attributes() { if(value) DeleteProcThreadAttributeList(value); }
};
void wincheck(bool value) { if(!value) throw std::runtime_error("appraisal worker process or pipe failure"); }
}
#endif
void WindowsAppraisalBoundary::supervise(std::shared_ptr<State> state, WindowsAppraisalOptions options) noexcept {
  try {
#ifdef _WIN32
    // Keep all handles and process operations on the supervisor. No caller joins
    // this thread; shared state remains alive until cancellation cleanup ends.
    auto run=[&] {
      if(state->abandoned.load()) return;
      std::array<unsigned char,16> random{};
      wincheck(BCryptGenRandom(nullptr,random.data(),static_cast<ULONG>(random.size()),BCRYPT_USE_SYSTEM_PREFERRED_RNG)==0);
      std::wstring name=L"\\\\.\\pipe\\vertex-appraisal-";
      for(auto byte:random) { name+=L"0123456789abcdef"[byte>>4]; name+=L"0123456789abcdef"[byte&15]; }
      Handle server(CreateNamedPipeW(name.c_str(),PIPE_ACCESS_DUPLEX | FILE_FLAG_FIRST_PIPE_INSTANCE,
          PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_NOWAIT | PIPE_REJECT_REMOTE_CLIENTS,1,65536,65536,0,nullptr));
      wincheck(server.valid());
      SECURITY_ATTRIBUTES security{sizeof(SECURITY_ATTRIBUTES),nullptr,TRUE};
      Handle client(CreateFileW(name.c_str(),GENERIC_READ | GENERIC_WRITE,0,&security,OPEN_EXISTING,0,nullptr));
      wincheck(client.valid());
      Handle null_error(CreateFileW(L"NUL",GENERIC_WRITE,FILE_SHARE_READ | FILE_SHARE_WRITE,&security,OPEN_EXISTING,0,nullptr));
      wincheck(null_error.valid());
      Handle job(CreateJobObjectW(nullptr,nullptr)); wincheck(job.valid());
      JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
      limits.BasicLimitInformation.LimitFlags=JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
      wincheck(SetInformationJobObject(job.value,JobObjectExtendedLimitInformation,&limits,sizeof(limits))!=FALSE);
      Attributes attributes;
      HANDLE inherited[]{client.value,null_error.value};
      wincheck(UpdateProcThreadAttribute(attributes.value,0,PROC_THREAD_ATTRIBUTE_HANDLE_LIST,inherited,sizeof(inherited),nullptr,nullptr)!=FALSE);
      STARTUPINFOEXW startup{}; startup.StartupInfo.cb=sizeof(startup);
      startup.StartupInfo.dwFlags=STARTF_USESTDHANDLES;
      startup.StartupInfo.hStdInput=client.value; startup.StartupInfo.hStdOutput=client.value;
      startup.StartupInfo.hStdError=null_error.value; startup.lpAttributeList=attributes.value;
      PROCESS_INFORMATION process{};
      auto command=L"\""+options.executable.wstring()+L"\"";
      // Avoid forwarding ambient credentials or application configuration in the
      // caller's environment. This is hygiene, not a Windows security boundary.
      std::array<wchar_t,32768> windows_directory{};
      const auto windows_length=GetWindowsDirectoryW(windows_directory.data(),static_cast<UINT>(windows_directory.size()));
      wincheck(windows_length!=0 && windows_length<windows_directory.size());
      const std::wstring windows_root(windows_directory.data(),windows_length);
      std::wstring environment;
      for(const auto &entry:{L"PATH="+windows_root+L"\\System32", L"SystemRoot="+windows_root, L"WINDIR="+windows_root}) {
        environment+=entry; environment.push_back(L'\0');
      }
      environment.push_back(L'\0');
      wincheck(CreateProcessW(options.executable.c_str(),command.data(),nullptr,nullptr,TRUE,
        CREATE_NO_WINDOW | CREATE_SUSPENDED | CREATE_UNICODE_ENVIRONMENT | EXTENDED_STARTUPINFO_PRESENT,environment.data(),
        options.executable.parent_path().c_str(),&startup.StartupInfo,&process)!=FALSE);
      Handle process_handle(process.hProcess), thread_handle(process.hThread);
      if(!AssignProcessToJobObject(job.value,process_handle.value)) {
        TerminateProcess(process_handle.value,1); throw std::runtime_error("job assignment failed");
      }
      if(state->abandoned.load()) return; // Closing job also kills suspended process.
      wincheck(ResumeThread(thread_handle.value)!=static_cast<DWORD>(-1));
      state->pid.store(process.dwProcessId);
      CloseHandle(client.value); client.value=nullptr;
      std::shared_ptr<std::string> outgoing;
      std::size_t written{};
      std::string incoming;
      bool awaiting{};
      unsigned responses{};
      while(!state->abandoned.load()) {
        if(!outgoing) {
          outgoing=state->outbound.exchange({});
          if(outgoing) { if(awaiting) { state->failure.store(2); return; } written=0; awaiting=true; }
        }
        if(outgoing) {
          DWORD n{};
          const auto count=static_cast<DWORD>(std::min<std::size_t>(16384,outgoing->size()-written));
          const bool ok=WriteFile(server.value,outgoing->data()+written,count,&n,nullptr)!=FALSE;
          if(!ok && GetLastError()!=ERROR_NO_DATA) { state->failure.store(1); return; }
          written+=n;
          if(written==outgoing->size()) outgoing.reset();
        }
        std::array<char,16384> buffer{}; DWORD n{};
        const bool read=ReadFile(server.value,buffer.data(),static_cast<DWORD>(buffer.size()),&n,nullptr)!=FALSE;
        const auto error=read ? ERROR_SUCCESS : GetLastError();
        if(n) {
          if(!awaiting || outgoing || incoming.size()+n>state->limit+4ULL) { state->failure.store(2); return; }
          incoming.append(buffer.data(),n);
          if(incoming.size()>=4) {
            std::uint32_t length{};
            for(unsigned i=0;i<4;++i) length|=static_cast<std::uint32_t>(static_cast<unsigned char>(incoming[i]))<<(i*8);
            if(!length || length>state->limit || incoming.size()>length+4ULL) { state->failure.store(2); return; }
            if(incoming.size()==length+4ULL) {
              try {
                auto reply=std::make_shared<AppraisalWorkerReply>(decode(incoming.substr(4)));
                std::shared_ptr<AppraisalWorkerReply> empty;
                if(!state->inbound.compare_exchange_strong(empty,std::move(reply))) { state->failure.store(2); return; }
              } catch(...) { state->failure.store(2); return; }
              incoming.clear(); awaiting=false;
              // The second complete response finishes the preparation exchange.
              // A worker may exit immediately after writing it. Do not race that
              // valid acknowledgement against a subsequent broken-pipe check.
              if(++responses==2) return;
            }
          }
        }
        if(!read && error!=ERROR_NO_DATA) { state->failure.store(incoming.empty()?1:2); return; }
        // Drain buffered bytes before classifying a process exit; a partial frame
        // is malformed, whereas an exit without any response is worker failure.
        if(!n && WaitForSingleObject(process_handle.value,0)==WAIT_OBJECT_0) {
          state->failure.store(incoming.empty()?1:2); return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
      // KILL_ON_JOB_CLOSE applies to descendants and handles launch/abandon races.
    };
    run();
#else
    (void)options;
    state->failure.store(1);
#endif
  } catch(...) { state->failure.store(1); }
  state->finished.store(true);
}
} // namespace sketch
