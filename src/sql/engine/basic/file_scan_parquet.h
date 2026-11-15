/* Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0. */
#pragma once
// Included by file_scan_reader.h after the bounded read-only Reader definition.
// Compact Thrift metadata and Parquet page decoding; no external SQL engine.
struct CompactValue {
  int64_t number=0;std::string text;
  std::vector<CompactValue> list;
  std::vector<int> field_ids;
  std::vector<CompactValue> fields;
  const CompactValue &get(int id) const;
};
inline const CompactValue &CompactValue::get(int id) const {
  for(size_t i=0;i<field_ids.size();++i) if(field_ids[i]==id) return fields[i];
  static const CompactValue empty;return empty;
}

class Compact {
public:
  Compact(const std::string &data):data_(data){}
  unsigned byte(){if(pos_>=data_.size()) throw Error("truncated Parquet metadata");return static_cast<unsigned char>(data_[pos_++]);}
  uint64_t varint(){uint64_t v=0;for(unsigned n=0;n<10;++n){unsigned b=byte();if(n==9 && b>1) throw Error("Parquet varint overflow");v|=uint64_t(b&127)<<(7*n);if(!(b&128)) return v;}throw Error("invalid Parquet varint");}
  int64_t zigzag(){uint64_t v=varint();return static_cast<int64_t>((v>>1)^(~(v&1)+1));}
  CompactValue value(unsigned type,unsigned depth=0){
    if(depth>32 || ++nodes_>1000000) throw Error("Parquet metadata resource limit");
    CompactValue v;
    if(type==1 || type==2) v.number=type==1;
    else if(type==3) v.number=static_cast<int8_t>(byte());
    else if(type>=4 && type<=6) v.number=zigzag();
    else if(type==7){for(int i=0;i<8;++i) byte();}
    else if(type==8){uint64_t n=varint();if(n>data_.size()-pos_) throw Error("invalid Parquet string length");v.text=data_.substr(pos_,n);pos_+=n;}
    else if(type==9 || type==10){unsigned b=byte();uint64_t n=b>>4;if(n==15)n=varint();if(n>100000) throw Error("Parquet collection limit");for(uint64_t i=0;i<n;++i)v.list.push_back(value(b&15,depth+1));}
    else if(type==11){uint64_t n=varint();if(n>100000)throw Error("Parquet map limit");if(n){unsigned t=byte();for(uint64_t i=0;i<n;++i){value(t>>4,depth+1);value(t&15,depth+1);}}}
    else if(type==12){int id=0;while(true){unsigned b=byte();if(!b)break;id=b>>4 ? id+(b>>4):zigzag();v.field_ids.push_back(id);v.fields.push_back(value(b&15,depth+1));}}
    else throw Error("invalid Parquet compact type");
    return v;
  }
  size_t position() const{return pos_;}
private:const std::string &data_;size_t pos_=0,nodes_=0;
};
inline uint64_t little(const std::string &s,size_t &p,unsigned bytes){
  if(bytes>8 || p>s.size() || bytes>s.size()-p)throw Error("truncated Parquet value");
  uint64_t v=0;for(unsigned i=0;i<bytes;++i)v|=uint64_t(static_cast<unsigned char>(s[p++]))<<(8*i);return v;
}
inline std::string snappy(const std::string &s,size_t expected){
  Compact c(s);uint64_t length=c.varint();size_t p=c.position();
  if(length!=expected || length>MAX_RECORD)throw Error("invalid Snappy output length");
  std::string out;out.reserve(length);
  while(p<s.size()){
    unsigned tag=static_cast<unsigned char>(s[p++]),kind=tag&3;size_t n=0,offset=0;
    if(kind==0){n=tag>>2;if(n<60)++n;else{unsigned bytes=n-59;n=little(s,p,bytes)+1;}if(n>s.size()-p || n>length-out.size())throw Error("invalid Snappy literal");out.append(s,p,n);p+=n;}
    else {
      if(kind==1){n=4+((tag>>2)&7);offset=((tag&0xe0)<<3)|little(s,p,1);}
      else{n=1+(tag>>2);offset=little(s,p,kind==2?2:4);}
      if(!offset || offset>out.size() || n>length-out.size())throw Error("invalid Snappy copy");
      for(size_t i=0;i<n;++i)out.push_back(out[out.size()-offset]);
    }
  }
  if(out.size()!=length)throw Error("truncated Snappy stream");
  return out;
}
inline std::vector<uint32_t> hybrid(const std::string &s,size_t &p,size_t end,unsigned width,size_t count){
  if(width>32 || end>s.size() || p>end || count>MAX_RECORD)throw Error("invalid Parquet levels");
  std::vector<uint32_t> out;out.reserve(count);
  while(out.size()<count){
    uint64_t head=0;unsigned shift=0;
    while(true){if(p>=end || shift>=64)throw Error("invalid Parquet RLE header");unsigned b=static_cast<unsigned char>(s[p++]);head|=uint64_t(b&127)<<shift;if(!(b&128))break;shift+=7;}
    if(!head)throw Error("zero Parquet RLE run");
    size_t n=head>>1;
    if(!(head&1)){
      unsigned bytes=(width+7)/8;if(p+bytes>end)throw Error("truncated Parquet RLE");uint32_t v=little(s,p,bytes);
      if(width<32 && v>=(uint64_t(1)<<width))throw Error("invalid Parquet level");
      size_t take=std::min(n,count-out.size());out.insert(out.end(),take,v);
    }else{
      if(n>MAX_RECORD/8)throw Error("Parquet bitpack resource limit");
      n*=8;
      size_t bytes=(n*width+7)/8;if(bytes>end-p)throw Error("truncated Parquet bitpack");
      for(size_t i=0;i<n && out.size()<count;++i){uint32_t v=0;for(unsigned j=0;j<width;++j){size_t bit=i*width+j;v|=((static_cast<unsigned char>(s[p+bit/8])>>(bit%8))&1)<<j;}out.push_back(v);}p+=bytes;
    }
  }
  return out;
}
inline std::string folded_parquet(std::string name){std::transform(name.begin(),name.end(),name.begin(),[](unsigned char c){return c>='A'&&c<='Z'?c+32:c;});return name;}
class Parquet {
  struct Chunk {uint64_t start=0,end=0;int physical=0,codec=0;int64_t count=0;};
  struct Group {int64_t rows=0;std::vector<Chunk> columns;};
  struct Stream {uint64_t offset=0;std::vector<Cell> dictionary,values;size_t index=0;};
public:
  Parquet(const Options &o,const Signature *expected=nullptr):reader_(o){reader_.open(expected);metadata();}
  void project(const std::vector<size_t> &cols){projection_=cols;projected_=true;}
  Schema schema()const{Schema s;s.columns=columns_;s.signature=reader_.signature();return s;}
  void reset(){group_=0;row_=0;streams_.clear();reader_.verify();}
  bool next(std::vector<Cell> &row){
    while(group_<groups_.size() && row_>=groups_[group_].rows){++group_;row_=0;streams_.clear();}
    if(group_==groups_.size()){reader_.verify();return false;}
    auto &g=groups_[group_];
    if(streams_.empty()){streams_.resize(columns_.size());for(size_t i=0;i<columns_.size();++i)streams_[i].offset=g.columns[i].start;}
    row.clear();size_t memory=0;
    for(size_t i=0;i<columns_.size();++i){
      if(projected_ && std::find(projection_.begin(),projection_.end(),i)==projection_.end()){row.push_back(Cell{});continue;}
      auto &st=streams_[i];while(st.index>=st.values.size()){st.values.clear();st.index=0;page(g.columns[i],st,i);}
      row.push_back(st.values[st.index++]);
      for(const auto &v:st.values)memory+=sizeof(Cell)+v.text.size();
      for(const auto &v:st.dictionary)memory+=sizeof(Cell)+v.text.size();
      if(memory>64*1024*1024)throw Error("Parquet page memory limit");
    }
    ++row_;reader_.verify();return true;
  }
private:
  void metadata(){
    auto sig=reader_.signature();if(sig.size<12 || reader_.read_at(0,4)!="PAR1")throw Error("invalid Parquet magic");
    std::string tail=reader_.read_at(sig.size-8,8);if(tail.substr(4)!="PAR1")throw Error("invalid Parquet footer magic");
    size_t p=0;uint64_t bytes=little(tail,p,4);if(bytes>16*1024*1024 || bytes>sig.size-12)throw Error("Parquet footer resource limit");
    std::string footer=reader_.read_at(sig.size-8-bytes,bytes);Compact compact(footer);auto meta=compact.value(12);
    auto &schema=meta.get(2).list;if(schema.size()<2 || schema.size()>MAX_COLUMNS+1 || schema[0].get(5).number!=static_cast<int64_t>(schema.size()-1))throw Error("only flat Parquet schema is supported");
    for(size_t i=1;i<schema.size();++i){
      auto &s=schema[i];if(s.get(3).number==2 || s.get(5).number)throw Error("nested/repeated Parquet schema is unsupported");
      Column c;c.name=s.get(4).text;c.nullable=s.get(3).number==1;
      if(c.name.empty() || c.name.size()>256 || !valid_utf8(c.name))throw Error("invalid Parquet column name");
      int physical=s.get(1).number;Type t=Type::Varchar;
      if(physical==0)t=Type::Boolean;else if(physical==1 || physical==2)t=Type::Bigint;else if(physical==4 || physical==5)t=Type::Double;else if(physical!=6)throw Error("unsupported Parquet physical type");
      // Converted DATE and TIMESTAMP types are preserved as SQL temporal types.
      if(s.get(6).number==6 && physical==1)t=Type::Date;
      if((s.get(6).number==9 || s.get(6).number==10) && physical==2)t=Type::Datetime;
      for(const auto &existing:columns_)if(folded_parquet(existing.name)==folded_parquet(c.name))throw Error("duplicate Parquet column name");
      for(size_t j=0;j<s.field_ids.size();++j)if(s.field_ids[j]==6){int64_t annotation=s.fields[j].number;if(annotation!=0 && annotation!=6 && annotation!=9 && annotation!=10)throw Error("unsupported Parquet converted type");}
      c.type=t;if(t==Type::Datetime)c.scale=s.get(6).number==9?3:6;columns_.push_back(c);physical_.push_back(physical);converted_.push_back(s.get(6).number);
    }
    for(auto &rg:meta.get(4).list){
      Group g;g.rows=rg.get(3).number;if(g.rows<0 || rg.get(1).list.size()!=columns_.size())throw Error("invalid Parquet row group");
      for(size_t i=0;i<columns_.size();++i){auto &cc=rg.get(1).list[i];if(!cc.get(1).text.empty())throw Error("external Parquet chunks are unsupported");auto &m=cc.get(3);
        if(m.get(3).list.size()!=1 || m.get(3).list[0].text!=columns_[i].name || m.get(1).number!=physical_[i])throw Error("Parquet chunk schema mismatch");
        Chunk c;c.physical=physical_[i];c.codec=m.get(4).number;c.count=m.get(5).number;
        int64_t data=m.get(9).number,dict=m.get(11).number,total=m.get(7).number;
        if(data<4 || dict<0 || total<0 || c.count!=g.rows)throw Error("invalid Parquet chunk bounds");
        c.start=dict>0?std::min(dict,data):data;c.end=c.start+total;
        if(c.end<c.start || c.end>sig.size-8-bytes || (c.codec!=0 && c.codec!=1))throw Error("unsupported Parquet compression or chunk bounds");
        g.columns.push_back(c);
      }
      groups_.push_back(std::move(g));if(groups_.size()>100000)throw Error("Parquet row group limit");
    }
  }
  std::vector<Cell> plain(const std::string &s,size_t &p,size_t count,size_t col){
    if(count>static_cast<int64_t>(MAX_RECORD/sizeof(Cell)))throw Error("Parquet page value count limit");
    std::vector<Cell> out;out.reserve(count);int type=physical_[col];
    for(size_t i=0;i<count;++i){Cell c;c.null=false;uint64_t v=0;
      if(type==0){size_t pos=p+i/8;if(pos>=s.size())throw Error("truncated Parquet bool");c.text=(static_cast<unsigned char>(s[pos])>>(i%8))&1?"true":"false";}
      else if(type==1 || type==2){v=little(s,p,type==1?4:8);int64_t n=type==1?static_cast<int32_t>(v):static_cast<int64_t>(v);
        if(columns_[col].type==Type::Date || columns_[col].type==Type::Datetime){
          if((columns_[col].type==Type::Date && (n < -719162 || n > 2932896)) ||
             (converted_[col]==9 && (n < -62135596800000LL || n > 253402300799999LL)))throw Error("Parquet date outside SQL range");
          int64_t micros=columns_[col].type==Type::Date?n*86400000000LL:converted_[col]==9?n*1000:n;
          time_t seconds=micros/1000000;struct tm tm{};if(!gmtime_r(&seconds,&tm))throw Error("invalid Parquet timestamp");char buf[64];
          std::strftime(buf,sizeof(buf),columns_[col].type==Type::Date?"%Y-%m-%d":"%Y-%m-%d %H:%M:%S",&tm);c.text=buf;
          if(columns_[col].type==Type::Datetime && micros%1000000){char sub[16];std::snprintf(sub,sizeof(sub),".%06lld",static_cast<long long>((micros%1000000+1000000)%1000000));c.text+=sub;}
        }else c.text=std::to_string(n);
      }else if(type==4 || type==5){v=little(s,p,type==4?4:8);double d;if(type==4){uint32_t bits=v;float f;std::memcpy(&f,&bits,4);d=f;}else std::memcpy(&d,&v,8);if(!std::isfinite(d))throw Error("nonfinite Parquet number");char buf[64];auto result=std::to_chars(buf,buf+sizeof(buf),d);c.text.assign(buf,result.ptr);}
      else{size_t n=little(s,p,4);if(n>MAX_RECORD || n>s.size()-p)throw Error("invalid Parquet binary length");c.text=s.substr(p,n);p+=n;if(!valid_utf8(c.text))throw Error("invalid Parquet UTF-8");}
      out.push_back(std::move(c));
    }
    if(type==0)p+=(count+7)/8;
    return out;
  }
  void page(const Chunk &chunk,Stream &st,size_t col){
    if(st.offset>=chunk.end)throw Error("Parquet chunk ended before its declared rows");
    std::string header=reader_.read_at(st.offset,std::min<uint64_t>(65536,chunk.end-st.offset));Compact compact(header);auto h=compact.value(12);size_t header_size=compact.position();
    int64_t compressed=h.get(3).number,uncompressed=h.get(2).number;
    if(compressed<0 || uncompressed<0 || compressed>static_cast<int64_t>(MAX_RECORD) || uncompressed>static_cast<int64_t>(MAX_RECORD) || header_size+compressed>chunk.end-st.offset)throw Error("invalid Parquet page bounds");
    std::string payload=reader_.read_at(st.offset+header_size,compressed);st.offset+=header_size+compressed;
    int type=h.get(1).number;
    if(type!=0 && type!=2 && type!=3)return;
    auto &dh=type==3?h.get(8):h.get(5);
    int64_t count=dh.get(1).number;
    int encoding=type==3?dh.get(4).number:dh.get(2).number;
    if(count<0 || count>static_cast<int64_t>(MAX_RECORD/sizeof(Cell)))throw Error("Parquet data page row limit");
    size_t prefix=0;
    std::string data;
    if(type==3){
      int64_t def=dh.get(5).number,rep=dh.get(6).number;
      if(def<0 || rep!=0 || static_cast<uint64_t>(def)>payload.size() || def>uncompressed)throw Error("invalid Parquet V2 levels");
      prefix=def;
      bool compressed_values=true;
      for(size_t j=0;j<dh.field_ids.size();++j)if(dh.field_ids[j]==7)compressed_values=dh.fields[j].number!=0;
      data=payload.substr(0,prefix);
      data+=chunk.codec==1 && compressed_values?snappy(payload.substr(prefix),uncompressed-prefix):payload.substr(prefix);
    }else data=chunk.codec==1?snappy(payload,uncompressed):payload;
    if(data.size()!=static_cast<size_t>(uncompressed))throw Error("Parquet page length mismatch");
    size_t p=0;
    if(type==2){if(h.get(7).get(2).number!=0)throw Error("unsupported Parquet dictionary encoding");st.dictionary=plain(data,p,h.get(7).get(1).number,col);return;}
    std::vector<uint32_t> levels(count,1);
    if(columns_[col].nullable){size_t n=type==3?prefix:little(data,p,4);if(n>data.size()-p)throw Error("invalid Parquet definition levels");size_t end=p+n;levels=hybrid(data,p,end,1,count);p=end;}
    size_t nonnull=0;for(auto v:levels)nonnull+=v!=0;
    std::vector<Cell> values;size_t decoded_bytes_=0;
    if(nonnull==0) {}
    else if(encoding==0)values=plain(data,p,nonnull,col);
    else if(encoding==2 || encoding==8){if(p>=data.size())throw Error("missing Parquet dictionary indices");unsigned width=static_cast<unsigned char>(data[p++]);auto indices=hybrid(data,p,data.size(),width,nonnull);for(auto v:indices){if(v>=st.dictionary.size())throw Error("invalid Parquet dictionary index");if(values.size()*sizeof(Cell)+decoded_bytes_+st.dictionary[v].text.size()>32*1024*1024)throw Error("Parquet decoded page size limit");decoded_bytes_+=st.dictionary[v].text.size();values.push_back(st.dictionary[v]);}}
    else throw Error("unsupported Parquet data encoding");
    size_t index=0;for(auto level:levels)st.values.push_back(level?values[index++]:Cell{});
  }
  bool projected_=false;std::vector<size_t> projection_;
  Reader reader_;std::vector<Column> columns_;std::vector<int> physical_,converted_;
  std::vector<Group> groups_;std::vector<Stream> streams_;size_t group_=0;int64_t row_=0;
};
