#include "ez3fs/live_cartridge_session.hpp"

#include <iostream>

namespace ez3fs {

LiveCartridgeSession::~LiveCartridgeSession() { std::string ignored;close(ignored); }

bool LiveCartridgeSession::open(std::string& error) {
    if(open_){error.clear();return true;}
    if(!storage_.openForLiveWrite(error))return false;
    unsigned displayed=101;
    const auto progress=[&displayed](std::size_t completed,std::size_t total) {
        const auto percent=total==0?100u:static_cast<unsigned>(completed*100/total);
        if(percent==displayed)return;
        displayed=percent;
        std::cerr<<"\rScanning EZ3FS-LIVE allocation: "<<percent<<'%'<<std::flush;
        if(completed==total)std::cerr<<'\n';
    };
    if(!live::Filesystem::open(device_,filesystem_,error,progress)){std::cerr<<'\n';std::string ignored;storage_.close(ignored);return false;}
    if(!filesystem_.verify(error)){std::string ignored;storage_.close(ignored);return false;}
    open_=true;error.clear();return true;
}

bool LiveCartridgeSession::close(std::string& error) {
    if(!open_){error.clear();return true;}
    open_=false;return storage_.close(error);
}

} // namespace ez3fs
