#pragma once
#include <string>
#include <vector>
#include <cstdint>
#include <fstream>
#include <regex>

inline bool json_slurp(const std::string& p, std::string& out){ std::ifstream ifs(p); if(!ifs) return false; out.assign((std::istreambuf_iterator<char>(ifs)),{}); return true; }

inline std::vector<uint8_t> json_extract_u8(const std::string& txt, const std::string& key){ std::vector<uint8_t> v; auto pos=txt.find("\""+key+"\""); if(pos==std::string::npos) return v; pos=txt.find('[',pos); if(pos==std::string::npos) return v; size_t end=pos+1; int d=1; while(end<txt.size()&&d>0){ if(txt[end]=='[') d++; else if(txt[end]==']') d--; ++end;} if(d) return v; std::string arr=txt.substr(pos+1,end-pos-2); std::regex re("(-?\\d+)"); for(auto it=std::sregex_iterator(arr.begin(), arr.end(), re); it!=std::sregex_iterator(); ++it){ long val=std::stol((*it)[1]); if(val>=0 && val<256) v.push_back((uint8_t)val);} return v; }
inline std::vector<uint16_t> json_extract_u16(const std::string& txt, const std::string& key){ std::vector<uint16_t> v; auto pos=txt.find("\""+key+"\""); if(pos==std::string::npos) return v; pos=txt.find('[',pos); if(pos==std::string::npos) return v; size_t end=pos+1; int d=1; while(end<txt.size()&&d>0){ if(txt[end]=='[') d++; else if(txt[end]==']') d--; ++end;} if(d) return v; std::string arr=txt.substr(pos+1,end-pos-2); std::regex re("(-?\\d+)"); for(auto it=std::sregex_iterator(arr.begin(), arr.end(), re); it!=std::sregex_iterator(); ++it){ long val=std::stol((*it)[1]); if(val>=0 && val<65536) v.push_back((uint16_t)val);} return v; }
