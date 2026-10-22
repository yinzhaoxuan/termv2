#include "uicfg/link_policy.h"
#include <cctype>
namespace uicfg {
static std::string sanitize(std::string_view u){
  std::string o; for(unsigned char c:u){ if(c<0x20||c==0x7f) { char b[8]; snprintf(b,sizeof b,"%%%02X",c); o+=b; } else o+=char(c);} return o; }
LinkCheck checkLink(std::string_view url){
  LinkCheck r{LinkDecision::RejectMalformed,"",sanitize(url)};
  auto p=url.find(':');
  if(p==std::string_view::npos||p==0) return r;
  std::string s;
  for(size_t i=0;i<p;i++){ unsigned char c=url[i];
    if(!(std::isalnum(c)||c=='+'||c=='-'||c=='.')||(i==0&&!std::isalpha(c))) return r;
    s+=char(std::tolower(c)); }
  r.scheme=s;
  for(unsigned char c:url) if(c<0x20||c==0x7f||c==' ') { r.decision=LinkDecision::RejectMalformed; return r; }
  if(s!="http"&&s!="https"){ r.decision=LinkDecision::RejectScheme; return r; }
  auto rest=url.substr(p+1);
  if(rest.substr(0,2)!="//"||rest.size()<3||rest[2]=='/') return r;
  r.decision=LinkDecision::Allow; return r;
}
}
