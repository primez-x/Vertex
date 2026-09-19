#include "sketch/field_adapter_contract.hpp"
#include <nlohmann/json.hpp>
#define NOMINMAX
#include <Windows.h>
#include <array>
#include <cstdint>
#include <string>

using Json = nlohmann::json;
bool exact_read(void *buffer, DWORD length) {
  auto p = static_cast<char *>(buffer);
  while (length) { DWORD n{}; if (!ReadFile(GetStdHandle(STD_INPUT_HANDLE), p, length, &n, nullptr) || !n) return false; p += n; length -= n; }
  return true;
}
void raw(const std::string &s) { DWORD n{}; WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), s.data(), static_cast<DWORD>(s.size()), &n, nullptr); }
void frame(const std::string &s) {
  std::string bytes(4, '\0');
  for (unsigned i=0; i<4; ++i) bytes[i] = static_cast<char>((s.size() >> (i*8)) & 255);
  raw(bytes + s);
}
int main(int argc, char **) {
  SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
  if (GetConsoleWindow()!=nullptr) return 19;
  if (GetEnvironmentVariableW(L"VERTEX_APPRAISAL_PARENT_SECRET",nullptr,0)!=0) return 22;
  if(argc>1) { Sleep(INFINITE); return 0; }
  for (int command=0; command<2; ++command) {
    std::array<unsigned char,4> header{};
    if (!exact_read(header.data(), 4)) return 2;
    std::uint32_t size{};
    for(unsigned i=0;i<4;++i) size |= static_cast<std::uint32_t>(header[i]) << (i*8);
    if (!size || size > 1024*1024) return 3;
    std::string payload(size, '\0'); if (!exact_read(payload.data(), size)) return 4;
    auto request=Json::parse(payload); auto id=request.at("request_id").get<std::string>();
    if(id=="crash") return 17;
    if(id=="hang") { Sleep(INFINITE); return 0; }
    if(id=="late") Sleep(2000);
    if(id=="malformed") { frame("not-json"); Sleep(INFINITE); }
    if(id=="oversize") { raw(std::string("\xff\xff\xff\x7f",4)); Sleep(INFINITE); }
    if(id=="duplicate") { frame("{\"version\":1,\"version\":1}"); Sleep(INFINITE); }
    if(id=="truncated") { raw(std::string("\x20\x00\x00\x00x",5)); return 0; }
    Json response={{"version",1},{"request_id",id=="wrong-id"?"unrelated":id}, {"capabilities",Json::array()}, {"prepared_payload",""}};
    if(command==0) {
      sketch::AppraisalMappingDescriptor d{{1,0},"fixture.appraisal","Synthetic Appraisal","fixture-1","fixture-json",
        "Synthetic preparation only",{{"area","gla",true}},3000};
      if(id=="tree") {
        std::wstring executable(32768,L'\0');
        const auto count=GetModuleFileNameW(nullptr,executable.data(),static_cast<DWORD>(executable.size()));
        if(!count || count>=executable.size()) return 20;
        executable.resize(count);
        auto child_command=L"\""+executable+L"\" --descendant";
        STARTUPINFOW startup{}; startup.cb=sizeof(startup);
        PROCESS_INFORMATION child{};
        if(!CreateProcessW(executable.c_str(),child_command.data(),nullptr,nullptr,FALSE,CREATE_NO_WINDOW,nullptr,nullptr,&startup,&child)) return 21;
        d.provenance=std::to_string(child.dwProcessId);
        CloseHandle(child.hThread); CloseHandle(child.hProcess);
      }
      response["kind"]="capabilities";
      response["capabilities"].push_back(Json::parse(sketch::appraisal_mapping_json(d)));
    } else { response["kind"]="prepared"; response["prepared_payload"]=request.at("payload"); }
    frame(response.dump());
    if(id=="no-read") Sleep(INFINITE);
    if(id=="exit-success" && command==1) return 0;
  }
  // Success is acknowledged before parent cleanup; remain alive to prove it.
  Sleep(INFINITE);
}
