#include "uicfg/link_policy.h"
#include <cstdio>
#include <cstdlib>
using namespace uicfg; int f=0;
#define T(u,d) do{ if(checkLink(u).decision!=LinkDecision::d){std::printf("FAIL %s\n",#u);f++;} }while(0)
int main(){
  T("https://example.com/a",Allow); T("HTTP://x.org",Allow);
  T("file:///C:/Windows/system32/cmd.exe",RejectScheme); T("mailto:a@b.c",RejectScheme);
  T("ms-settings:",RejectScheme); T("javascript:alert(1)",RejectScheme); T("vscode://x",RejectScheme);
  T("",RejectMalformed); T("https:",RejectMalformed); T("https:///x",RejectMalformed);
  T("https://a b",RejectMalformed); T("https://a\nb",RejectMalformed); T("1http://x",RejectMalformed);
  if(checkLink("https://a\x1b[2Jb").displayUrl.find('\x1b')!=std::string::npos){std::puts("FAIL esc");f++;}
  std::printf("link_policy_tests: %s\n",f?"FAIL":"OK"); return f?1:0; }
