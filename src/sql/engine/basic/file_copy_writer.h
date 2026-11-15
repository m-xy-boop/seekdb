/* Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0. */
#pragma once
// file_scan_reader.h must be included first.
namespace oceanbase { namespace sql { namespace file_sql {
class CopyWriter {
public:
  CopyWriter(const Options &o):options_(o){
    struct ConstructionGuard {
      CopyWriter &w;bool done=false;
      ~ConstructionGuard(){if(!done){if(w.fd_>=0){::close(w.fd_);w.fd_=-1;}if(w.dirfd_>=0){if(!w.temp_.empty())unlinkat(w.dirfd_,w.temp_.c_str(),0);::close(w.dirfd_);w.dirfd_=-1;}}}
    } guard{*this};
    if(o.path.find('\0')!=std::string::npos || o.path.empty())throw Error("invalid COPY output path");
    if(!o.delimiter || !o.quote || !o.escape || o.delimiter==o.quote || o.delimiter=='\n' || o.delimiter=='\r' ||
       o.quote=='\n' || o.quote=='\r' || o.nullstr.find_first_of(std::string()+o.delimiter+o.quote+'\n'+'\r')!=std::string::npos)
      throw Error("invalid COPY CSV dialect or NULL text");
    size_t slash=o.path.rfind('/');std::string dir=canonical(slash==std::string::npos?".":o.path.substr(0,slash+1));
    directory_=dir;
    filename_=slash==std::string::npos?o.path:o.path.substr(slash+1);
    if(filename_.empty() || filename_=="." || filename_=="..")throw Error("invalid COPY output name");
    check_path(o.secure_root,dir+"/"+filename_);
    dirfd_=::open(dir.c_str(),O_DIRECTORY|O_RDONLY|O_CLOEXEC|O_NOFOLLOW);
    if(dirfd_<0)throw Error("cannot open COPY output directory");
    char link[64],target[4096];std::snprintf(link,sizeof(link),"/proc/self/fd/%d",dirfd_);
    ssize_t n=::readlink(link,target,sizeof(target)-1);
    if(n<0)throw Error("cannot verify COPY output directory");
    target[n]=0;
    if(dir!=target)throw Error("COPY directory changed while opening");
    check_path(options_.secure_root,std::string(target)+"/"+filename_);
    // Temp names are allocated exclusively; final publication uses linkat without overwrite.
    for(unsigned i=0;i<100;++i){temp_=".seekdb-copy-"+std::to_string(getpid())+"-"+std::to_string(reinterpret_cast<uintptr_t>(this))+"-"+std::to_string(i);fd_=openat(dirfd_,temp_.c_str(),O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC|O_NOFOLLOW,0600);if(fd_>=0)break;}
    if(fd_<0)throw Error("cannot create COPY temporary file");
    guard.done=true;
  }
  ~CopyWriter(){if(fd_>=0)::close(fd_);if(dirfd_>=0){if(!temp_.empty())unlinkat(dirfd_,temp_.c_str(),0);::close(dirfd_);}}
  CopyWriter(const CopyWriter &)=delete;
  void row(const std::vector<Cell> &row){
    std::string line;
    for(size_t i=0;i<row.size();++i){if(i)line.push_back(options_.delimiter);auto &v=row[i];
      if(v.null)line+=options_.nullstr;
      else {
        bool quote=v.text.empty() || v.text==options_.nullstr || v.text.find_first_of(std::string()+options_.delimiter+options_.quote+'\n'+'\r')!=std::string::npos;
        if(quote)line.push_back(options_.quote);
        for(char c:v.text){if(quote && (c==options_.quote || c==options_.escape))line.push_back(options_.escape);line.push_back(c);}
        if(quote)line.push_back(options_.quote);
      }
      if(line.size()>MAX_RECORD)throw Error("COPY output record exceeds limit");
    }
    line.push_back('\n');size_t done=0;
    while(done<line.size()){ssize_t n=::write(fd_,line.data()+done,line.size()-done);if(n<=0)throw Error("COPY write failed");done+=n;}
  }
  void finish(){
    char link[64],target[4096];std::snprintf(link,sizeof(link),"/proc/self/fd/%d",dirfd_);
    ssize_t n=::readlink(link,target,sizeof(target)-1);
    if(n<0)throw Error("cannot revalidate COPY directory");
    target[n]=0;
    if(directory_!=target)throw Error("COPY directory changed during export");
    check_path(options_.secure_root,std::string(target)+"/"+filename_);
    if(::fsync(fd_))throw Error("COPY flush failed");
    if(::linkat(dirfd_,temp_.c_str(),dirfd_,filename_.c_str(),0))throw Error("COPY target exists or cannot publish output");
    unlinkat(dirfd_,temp_.c_str(),0);temp_.clear();
  }
private:Options options_;int fd_=-1,dirfd_=-1;std::string temp_,filename_,directory_;
};
}}}
