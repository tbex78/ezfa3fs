#include "ez3fs/live_cartridge_session.hpp"

#include <iostream>
#include <utility>

namespace ez3fs {

LiveCartridgeSession::~LiveCartridgeSession() { std::string ignored;close(ignored); }

bool LiveCartridgeSession::open(
    std::string& error,bool verify_referenced_data,
    live::Filesystem::ScanProgress verification_progress) {
    if(open_){error.clear();return true;}
    if(!storage_.openForLiveWrite(error))return false;
    if(!live::Filesystem::open(device_,filesystem_,error)){std::string ignored;storage_.close(ignored);return false;}
    if(verify_referenced_data&&
       !filesystem_.verify(error,std::move(verification_progress))){
        std::string ignored;storage_.close(ignored);return false;
    }
    open_=true;error.clear();return true;
}

bool LiveCartridgeSession::close(std::string& error) {
    if(!open_){error.clear();return true;}
    open_=false;return storage_.close(error);
}

} // namespace ez3fs
