#pragma once

#include <algorithm>
#include <any>
#include <arpa/inet.h>
#include <cassert>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <cstdio>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <shared_mutex>
#include <thread>
#include <chrono>
#include <atomic>
#include <list>
#include <map>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

using qint8=int8_t; using quint8=uint8_t; using qint16=int16_t; using quint16=uint16_t;
using qint32=int32_t; using quint32=uint32_t; using qint64=int64_t; using quint64=uint64_t;
using uint=unsigned int; using uchar=unsigned char; using ushort=unsigned short; using ulong=unsigned long; using qulonglong=unsigned long long;
template<class T> inline std::enable_if_t<std::is_integral_v<T>, uint> qHash(T value){return uint(std::hash<T>{}(value));}
#define Q_ASSERT(x) assert(x)
#define Q_ASSERT_X(x,w,m) assert(x)
#define QT_VERSION 0x050700
#define Q_OBJECT
#define Q_PROPERTY(...)
#define Q_ENUMS(...)
#define Q_FLAG(...)
#define Q_DECLARE_METATYPE(...)
#define slots
#define signals public
#define emit
#define Q_UNUSED(x) (void)(x)
#define qPrintable(x) ((x).constData())
#define qUtf8Printable(x) ((x).toUtf8().constData())
#define foreach(variable, container) for(variable : container)

template<class E> class QFlags { int v_=0; public: constexpr QFlags()=default; constexpr QFlags(E e):v_(int(e)){} constexpr QFlags(int v):v_(v){} operator int()const{return v_;} bool testFlag(E e)const{return (v_&int(e))==int(e) && (int(e)!=0||v_==0);} QFlags& operator|=(E e){v_|=int(e);return *this;} QFlags& operator|=(QFlags e){v_|=e.v_;return *this;} QFlags& operator&=(int v){v_&=v;return *this;} friend QFlags operator|(QFlags a,QFlags b){return QFlags(a.v_|b.v_);} };
#define Q_DECLARE_FLAGS(Name, Enum) using Name = QFlags<Enum>
#define Q_DECLARE_OPERATORS_FOR_FLAGS(Name)

class QChar { char c_=0; public: QChar()=default; QChar(char c):c_(c){} char toLatin1()const{return c_;} bool isUpper()const{return std::isupper(static_cast<unsigned char>(c_));} operator char()const{return c_;} };
class QString;
class QStringList;
class QRegExp { std::string p_; bool wildcard_=false; mutable std::vector<std::string> captures_; public: enum PatternSyntax{RegExp,Wildcard}; QRegExp()=default; explicit QRegExp(const char*p):p_(p){} explicit QRegExp(const std::string&p):p_(p){} void setPatternSyntax(PatternSyntax s){wildcard_=s==Wildcard;} void setPattern(const QString&p); bool exactMatch(const QString&text)const; const std::string& pattern()const{return p_;} int indexIn(const QString&text)const; QString cap(int index)const; };

class QByteArray {
 std::string s_;
public:
 QByteArray()=default; QByteArray(const char*s):s_(s?s:""){} QByteArray(const char*s,int n):s_(s,n){} QByteArray(int n,char c):s_(n,c){}
 int size()const{return int(s_.size());} int length()const{return size();} bool isEmpty()const{return s_.empty();} void clear(){s_.clear();} void resize(int n){s_.resize(n);}
 char* data(){return s_.empty()?nullptr:s_.data();} const char* data()const{return s_.data();} const char* constData()const{return s_.c_str();}
 char& operator[](int i){return s_[i];} char operator[](int i)const{return s_[i];} char at(int i)const{return s_.at(i);} QByteArray& fill(char c,int n=-1){if(n>=0)s_.resize(n);std::fill(s_.begin(),s_.end(),c);return *this;}
 QByteArray& append(const QByteArray&o){s_+=o.s_;return *this;} QByteArray& append(char c){s_+=c;return *this;} QByteArray& append(const char*p,int n){s_.append(p,n);return *this;}
 QByteArray& remove(int pos,int len){s_.erase(pos,len);return *this;}
 QByteArray& insert(int pos,const QByteArray&o){s_.insert(pos,o.s_);return *this;}
 QByteArray& prepend(const QByteArray&o){s_.insert(0,o.s_);return *this;} QByteArray mid(int p,int n=-1)const{return QByteArray(s_.substr(p,n<0?std::string::npos:n).data(), int(s_.substr(p,n<0?std::string::npos:n).size()));}
 QByteArray left(int n)const{return mid(0,n);} QByteArray right(int n)const{return mid(std::max(0,size()-n));} QByteArray toHex()const { static const char*h="0123456789abcdef"; QByteArray r; for(unsigned char c:s_){r.append(h[c>>4]);r.append(h[c&15]);} return r; }
 static QByteArray fromHex(const QByteArray& a){QByteArray r; for(int i=0;i+1<a.size();i+=2){auto cv=[](char c){return c<='9'?c-'0':std::tolower(c)-'a'+10;};r.append(char((cv(a[i])<<4)|cv(a[i+1])));}return r;}
 operator std::string()const{return s_;} bool operator==(const QByteArray&o)const{return s_==o.s_;} bool operator!=(const QByteArray&o)const{return !(*this==o);} QByteArray& operator+=(const QByteArray&o){return append(o);}
};
inline uint qHash(const QByteArray&a){return uint(std::hash<std::string>{}(std::string(a)));}

class QString {
 std::string s_;
 static std::string numu(quint64 v,int base){std::ostringstream o;if(base==16)o<<std::hex;else if(base==8)o<<std::oct;o<<v;return o.str();}
public:
 QString()=default; QString(const char*s):s_(s?s:""){} QString(const std::string&s):s_(s){} QString(const QByteArray&a):s_(a.constData(),a.size()){}
 static QString fromStdString(const std::string&s){return QString(s);} static QString fromUtf8(const char*s){return QString(s);} static QString fromLatin1(const char*s){return QString(s);} std::string toStdString()const{return s_;} QByteArray toUtf8()const{return QByteArray(s_.data(),s_.size());}
 const char* constData()const{return s_.c_str();} const char* c_str()const{return s_.c_str();} bool isEmpty()const{return s_.empty();} bool isNull()const{return s_.empty();} int size()const{return int(s_.size());} int length()const{return size();} int count(QChar c)const{return int(std::count(s_.begin(),s_.end(),char(c)));} void clear(){s_.clear();} QChar at(int i)const{return QChar(s_.at(i));} QString& append(QChar c){s_+=char(c);return *this;} QString& append(const QString&o){s_+=o.s_;return *this;}
 QString arg(const QString&v)const {QString r=*this; std::smatch m; std::regex p("%[1-9]");if(std::regex_search(r.s_,m,p))r.s_.replace(m.position(),m.length(),v.s_);return r;} QString arg(const QString&a,const QString&b)const{return arg(a).arg(b);}
 QString arg(const char*v)const{return arg(QString(v));} QString arg(qint64 v,int width=0,int base=10,QChar fill=' ')const{return arg(QString::number(v,base).rightJustified(width,fill));} QString arg(quint64 v,int width=0,int base=10,QChar fill=' ')const{return arg(QString::number(v,base).rightJustified(width,fill));} QString arg(int v,int w=0,int b=10,QChar f=' ')const{return arg(qint64(v),w,b,f);} QString arg(uint v,int w=0,int b=10,QChar f=' ')const{return arg(quint64(v),w,b,f);}
 static QString number(qint64 v,int base=10){if(v<0)return QString("-")+QString(numu(-v,base));return QString(numu(v,base));} static QString number(quint64 v,int base=10){return QString(numu(v,base));} static QString number(int v,int base=10){return number(qint64(v),base);} static QString number(uint v,int base=10){return number(quint64(v),base);}
 QString& setNum(qint64 v,int b=10){s_=number(v,b).s_;return *this;} QString rightJustified(int w,QChar f=' ')const{return size()>=w?*this:QString(std::string(w-size(),char(f))+s_);}
 QString toUpper()const{QString r=*this;for(char&c:r.s_)c=std::toupper(c);return r;} QString toLower()const{QString r=*this;for(char&c:r.s_)c=std::tolower(c);return r;}
 bool endsWith(QChar c)const{return !s_.empty()&&s_.back()==char(c);} void chop(int n){if(n>0)s_.resize(n>=size()?0:s_.size()-size_t(n));} QStringList split(QChar separator)const;
 QString& prepend(const QString&o){s_.insert(0,o.s_);return*this;} QString& insert(int pos,const QString&o){s_.insert(pos,o.s_);return *this;} QString& remove(QChar c){s_.erase(std::remove(s_.begin(),s_.end(),char(c)),s_.end());return *this;} QString& remove(const QString&text){for(size_t p=0;(p=s_.find(text.s_,p))!=std::string::npos;)s_.erase(p,text.s_.size());return*this;} QString& replace(QChar before,QChar after){std::replace(s_.begin(),s_.end(),char(before),char(after));return*this;} QString& replace(QChar before,const QString&after){for(size_t p=0;(p=s_.find(char(before),p))!=std::string::npos;p+=after.s_.size())s_.replace(p,1,after.s_);return*this;} QString& replace(const QRegExp&r,const QString&with){s_=std::regex_replace(s_,std::regex(r.pattern()),with.s_);return *this;} QString& replace(const char*before,const char*after){for(size_t p=0;(p=s_.find(before,p))!=std::string::npos;p+=std::strlen(after))s_.replace(p,std::strlen(before),after);return *this;} QString& replace(const char*before,const QString&after){return replace(before,after.c_str());} bool contains(const QRegExp&r)const{return std::regex_search(s_,std::regex(r.pattern()));} bool contains(const char *text)const{return s_.find(text)!=std::string::npos;}
 uint toUInt(bool*ok=nullptr,int base=10)const{try{auto v=std::stoul(s_,nullptr,base);if(ok)*ok=true;return uint(v);}catch(...){if(ok)*ok=false;return 0;}} quint64 toULongLong(bool*ok=nullptr,int base=10)const{try{auto v=std::stoull(s_,nullptr,base);if(ok)*ok=true;return v;}catch(...){if(ok)*ok=false;return 0;}} int toInt(bool*ok=nullptr,int base=10)const{try{auto v=std::stoi(s_,nullptr,base);if(ok)*ok=true;return v;}catch(...){if(ok)*ok=false;return 0;}} double toDouble(bool*ok=nullptr)const{try{auto v=std::stod(s_);if(ok)*ok=true;return v;}catch(...){if(ok)*ok=false;return 0;}}
 QByteArray toLatin1()const{return QByteArray(s_.data(),s_.size());} bool operator==(const QString&o)const{return s_==o.s_;} bool operator==(const char*o)const{return s_==(o?o:"");} friend bool operator==(const char*a,const QString&b){return b==a;} bool operator<(const QString&o)const{return s_<o.s_;} QString& operator+=(const QString&o){s_+=o.s_;return *this;} friend QString operator+(QString a,const QString&b){return a+=b;}
};
inline uint qHash(const QString&s){return uint(std::hash<std::string>{}(s.toStdString()));}
inline void QRegExp::setPattern(const QString&p){p_=p.toStdString();}
inline bool QRegExp::exactMatch(const QString&text)const{std::string expression;if(wildcard_){expression="^";for(char c:p_){if(c=='*')expression+=".*";else if(c=='?')expression+='.';else{if(std::strchr(".^$|()[]{}+\\",c))expression+='\\';expression+=c;}}expression+='$';}else expression=p_;return std::regex_match(text.toStdString(),std::regex(expression));}
inline int QRegExp::indexIn(const QString&text)const{std::string subject(text.c_str());std::smatch match;captures_.clear();if(!std::regex_search(subject,match,std::regex(p_)))return -1;for(const auto&part:match)captures_.push_back(part.str());return int(match.position());}
inline QString QRegExp::cap(int index)const{return index>=0&&size_t(index)<captures_.size()?QString(captures_[index]):QString();}

template<class T> class QList:public std::vector<T>{public:using std::vector<T>::vector; int size()const{return int(std::vector<T>::size());} void append(const T&v){this->push_back(v);} void prepend(const T&v){this->insert(this->begin(),v);} void replace(int i,const T&v){(*this)[i]=v;} T value(int i,const T&d=T())const{return i>=0&&i<size()?(*this)[i]:d;} QList& operator<<(const T&v){append(v);return *this;} QList& operator+=(const QList&o){this->insert(this->end(),o.begin(),o.end());return *this;} int count()const{return size();} int removeAll(const T&v){const int old=size();this->erase(std::remove(this->begin(),this->end(),v),this->end());return old-size();} bool isEmpty()const{return this->empty();} T& first(){return this->front();}const T&first()const{return this->front();} T& last(){return this->back();} T takeFirst(){T v=this->front();this->erase(this->begin());return v;} T takeAt(int i){T v=(*this)[i];this->erase(this->begin()+i);return v;} bool contains(const T&v)const{return std::find(this->begin(),this->end(),v)!=this->end();}};
template<class T> using QVector=QList<T>;
class QStringList:public QList<QString>{public:using QList<QString>::QList;QStringList()=default;QStringList(const QList<QString>&v){this->assign(v.begin(),v.end());}QStringList&operator<<(const QString&v){append(v);return*this;}QString join(const QString&separator)const{QString out;for(int i=0;i<size();++i){if(i)out.append(separator);out.append(at(i));}return out;}};
inline QStringList QString::split(QChar separator)const{QStringList result;std::string value=s_;size_t start=0,pos;while((pos=value.find(char(separator),start))!=std::string::npos){result<<QString(value.substr(start,pos-start));start=pos+1;}result<<QString(value.substr(start));return result;}
template<class T> class QSet:public std::set<T>{public:using std::set<T>::set; bool contains(const T&v)const{return this->count(v)!=0;} bool isEmpty()const{return this->empty();} void remove(const T&v){this->erase(v);} QSet& operator<<(const T&v){this->insert(v);return *this;}};
template<class K,class V> class QMap:public std::map<K,V>{public:using Base=std::map<K,V>;using Base::Base; void insert(const K&k,const V&v){(*this)[k]=v;} bool contains(const K&k)const{return this->count(k)!=0;} V value(const K&k,const V&d=V())const{auto i=this->find(k);return i==this->end()?d:i->second;} V take(const K&k){auto i=this->find(k);if(i==this->end())return V();V v=i->second;this->erase(i);return v;} QList<K> keys()const{QList<K> r;for(auto&p:*this)r<<p.first;return r;} QList<V> values()const{QList<V>r;for(auto&p:*this)r<<p.second;return r;} };
template<class K,class V> class QMultiMap:public std::multimap<K,V>{public:using std::multimap<K,V>::multimap;void insert(const K&k,const V&v){this->emplace(k,v);}bool contains(const K&k,const V&v)const{auto r=this->equal_range(k);for(auto i=r.first;i!=r.second;++i)if(i->second==v)return true;return false;}};
template<class K> struct QtFreeHash {size_t operator()(const K&k)const{if constexpr(std::is_base_of_v<QByteArray,K>)return qHash(static_cast<const QByteArray&>(k));else return qHash(k);}};
template<class K,class V> class QHash:public std::unordered_map<K,V,QtFreeHash<K>>{public:using Base=std::unordered_map<K,V,QtFreeHash<K>>;using Base::Base; auto constBegin()const{return this->cbegin();} auto constEnd()const{return this->cend();} void insert(const K&k,const V&v){(*this)[k]=v;} bool contains(const K&k)const{return this->count(k)!=0;} bool isEmpty()const{return this->empty();} void remove(const K&k){this->erase(k);} V take(const K&k){auto i=this->find(k);if(i==this->end())return V();V v=i->second;this->erase(i);return v;} V value(const K&k,const V&d=V())const{auto i=this->find(k);return i==this->end()?d:i->second;} QList<K> keys()const{QList<K>r;for(const auto&p:*this)r<<p.first;return r;} QList<K> uniqueKeys()const{return keys();} QList<V> values()const{QList<V>r;for(const auto&p:*this)r<<p.second;return r;}};
template<class K,class V> class QHashIterator {using Hash=QHash<K,V>;const Hash&h_;typename Hash::const_iterator next_,cur_;public:explicit QHashIterator(const Hash&h):h_(h),next_(h.begin()),cur_(h.end()){}bool hasNext()const{return next_!=h_.end();}void next(){cur_=next_++;}const K&key()const{return cur_->first;}const V&value()const{return cur_->second;}};
template<class K,class V> class QMutableHashIterator {using Hash=QHash<K,V>;Hash&h_;typename Hash::iterator next_,cur_;bool valid_=false;public:explicit QMutableHashIterator(Hash&h):h_(h),next_(h.begin()),cur_(h.end()){}bool hasNext()const{return next_!=h_.end();}void next(){cur_=next_++;valid_=true;}const K&key()const{return cur_->first;}V&value(){return cur_->second;}void remove(){if(valid_){h_.erase(cur_);valid_=false;}}};
template<class T> class QLinkedList:public std::list<T>{public:using std::list<T>::list; void append(const T&v){this->push_back(v);} int count()const{return int(this->size());} bool isEmpty()const{return this->empty();} T& first(){return this->front();} T& last(){return this->back();} T takeFirst(){T v=this->front();this->pop_front();return v;} bool contains(const T&v)const{return std::find(this->begin(),this->end(),v)!=this->end();}};
template<class T> class QMutableLinkedListIterator {
 QLinkedList<T>&l_; typename QLinkedList<T>::iterator next_,cur_; bool valid_=false;
public:
 explicit QMutableLinkedListIterator(QLinkedList<T>&l):l_(l),next_(l.begin()),cur_(l.end()){}
 bool hasNext()const{return next_!=l_.end();}
 bool hasPrevious()const{return next_!=l_.begin();}
 T& next(){cur_=next_++;valid_=true;return *cur_;}
 T& previous(){cur_=--next_;valid_=true;return *cur_;}
 T& peekNext()const{return *next_;}
 T& peekPrevious()const{auto i=next_;return *--i;}
 bool findNext(const T&v){while(hasNext())if(next()==v)return true;return false;}
 bool findPrevious(const T&v){while(hasPrevious())if(previous()==v)return true;return false;}
 void insert(const T&v){next_=l_.insert(next_,v);++next_;valid_=false;}
 void remove(){if(valid_){if(cur_==next_)++next_;l_.erase(cur_);cur_=l_.end();valid_=false;}}
 T& value(){return *cur_;} const T& value()const{return *cur_;}
 void setValue(const T&v){*cur_=v;}
 void toBack(){next_=l_.end();cur_=l_.end();valid_=false;}
 void toFront(){next_=l_.begin();cur_=l_.end();valid_=false;}
};

class QVariant; using QVariantList=QList<QVariant>; using QVariantMap=QMap<QString,QVariant>;
class QVariant {using Data=std::variant<std::monostate,bool,qint64,quint64,double,QString,QByteArray,QStringList,QVariantList,QVariantMap>;Data d_;std::any custom_;public:QVariant()=default;QVariant(bool v):d_(v){} QVariant(int v):d_(qint64(v)){} QVariant(uint v):d_(quint64(v)){} QVariant(qint64 v):d_(v){} QVariant(quint64 v):d_(v){} QVariant(const QString&v):d_(v){} QVariant(const char*v):d_(QString(v)){} QVariant(const QByteArray&v):d_(v){} QVariant(const QStringList&v):d_(v){} QVariant(const QVariantList&v):d_(v){} QVariant(const QVariantMap&v):d_(v){} bool isValid()const{return d_.index()!=0||custom_.has_value();} int type()const{return int(d_.index());} const char* typeName()const{return custom_.has_value()?"UInt128":"";} int toInt(bool*ok=nullptr)const{if(ok)*ok=isValid();return int(toLongLong());} uint toUInt(bool*ok=nullptr)const{if(ok)*ok=isValid();return uint(toULongLong());} qint64 toLongLong(bool*ok=nullptr)const{if(ok)*ok=isValid();return std::visit([](auto&&v)->qint64{using T=std::decay_t<decltype(v)>;if constexpr(std::is_arithmetic_v<T>)return v;else if constexpr(std::is_same_v<T,QString>)return v.toULongLong();else return 0;},d_);} quint64 toULongLong(bool*ok=nullptr)const{if(ok)*ok=isValid();return quint64(toLongLong());} bool toBool()const{return toLongLong()!=0;} QString toString()const{if(auto*p=std::get_if<QString>(&d_))return *p;return QString::number(toLongLong());} QByteArray toByteArray()const{auto*p=std::get_if<QByteArray>(&d_);return p?*p:QByteArray();} QStringList toStringList()const{auto*p=std::get_if<QStringList>(&d_);return p?*p:QStringList();} QVariantList toList()const{auto*p=std::get_if<QVariantList>(&d_);return p?*p:QVariantList();} QVariantMap toMap()const{auto*p=std::get_if<QVariantMap>(&d_);return p?*p:QVariantMap();} template<class T>void setValue(const T&v){if constexpr(std::is_integral_v<T>){if constexpr(std::is_signed_v<T>)d_=qint64(v);else d_=quint64(v);}else custom_=v;} template<class T>T value()const{if constexpr(std::is_integral_v<T>)return T(toULongLong());else if(auto*p=std::any_cast<T>(&custom_))return *p;else return T();} template<class T>static QVariant fromValue(const T&v){QVariant r;r.setValue(v);return r;}};

class QObject {public:explicit QObject(QObject * = nullptr){} virtual ~QObject()=default;static QString tr(const char*s){return QString(s);}};
struct Q_IPV6ADDR {quint8 c[16]{};quint8&operator[](int i){return c[i];}quint8 operator[](int i)const{return c[i];}};
class QAbstractSocket {public:enum NetworkLayerProtocol{UnknownNetworkLayerProtocol=-1,IPv4Protocol=0,IPv6Protocol=1};};
class QHostAddress {int proto_=0;quint32 v4_=0;Q_IPV6ADDR v6_{};public:QHostAddress()=default;explicit QHostAddress(quint32 v):proto_(4),v4_(v){}explicit QHostAddress(const Q_IPV6ADDR&v):proto_(6),v6_(v){}explicit QHostAddress(const quint8*v):proto_(6){std::memcpy(v6_.c,v,16);}explicit QHostAddress(const QString&s){if(inet_pton(AF_INET,s.c_str(),&v4_)==1){proto_=4;v4_=ntohl(v4_);}else if(inet_pton(AF_INET6,s.c_str(),v6_.c)==1)proto_=6;}QString toString()const{char b[INET6_ADDRSTRLEN]{};if(proto_==4){quint32 n=htonl(v4_);inet_ntop(AF_INET,&n,b,sizeof b);}else if(proto_==6)inet_ntop(AF_INET6,v6_.c,b,sizeof b);return QString(b);}quint32 toIPv4Address()const{return v4_;}Q_IPV6ADDR toIPv6Address()const{return v6_;}QAbstractSocket::NetworkLayerProtocol protocol()const{return proto_==4?QAbstractSocket::IPv4Protocol:proto_==6?QAbstractSocket::IPv6Protocol:QAbstractSocket::UnknownNetworkLayerProtocol;}};
struct QDebugBase {bool hexadecimal;}; constexpr QDebugBase hex{true},dec{false};
class QDebug {std::ostream*o_;public:explicit QDebug(std::ostream&o):o_(&o){}~QDebug(){*o_<<'\n';}QDebug& maybeSpace(){return *this;}QDebug&operator<<(QDebugBase base){if(base.hexadecimal)*o_<<std::hex;else *o_<<std::dec;return*this;}template<class T>QDebug&operator<<(const QList<T>&v){*o_<<'(';for(int i=0;i<v.size();++i){if(i)*o_<<',';*o_<<v[i];}*o_<<") ";return*this;}template<class T>QDebug&operator<<(const T&v){*o_<<v<<' ';return*this;}QDebug&operator<<(const QString&v){*o_<<v.c_str()<<' ';return*this;}QDebug&operator<<(const QByteArray&v){o_->write(v.data(),v.size());return*this;}};
class QDebugStateSaver {public:explicit QDebugStateSaver(QDebug&){}};
inline QDebug qDebug(){return QDebug(std::cerr);}inline QDebug qWarning(){return QDebug(std::cerr);}
template<class...A>inline void qDebug(const char*f,A...a){std::fprintf(stderr,f,a...);std::fputc('\n',stderr);}template<class...A>inline void qWarning(const char*f,A...a){std::fprintf(stderr,f,a...);std::fputc('\n',stderr);}template<class...A>[[noreturn]]inline void qFatal(const char*f,A...a){std::fprintf(stderr,f,a...);std::fputc('\n',stderr);std::abort();}inline int qrand(){return std::rand();}
template<class T> inline T qMin(T a,T b){return std::min(a,b);} template<class T>inline T qMax(T a,T b){return std::max(a,b);}
inline void qsrand(uint seed){std::srand(seed);}
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
template<class T>inline T qToBigEndian(T v){if constexpr(sizeof(T)==1)return v;else if constexpr(sizeof(T)==2)return T(__builtin_bswap16(uint16_t(v)));else if constexpr(sizeof(T)==4)return T(__builtin_bswap32(uint32_t(v)));else if constexpr(sizeof(T)==8)return T(__builtin_bswap64(uint64_t(v)));else return v;}
#elif defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
template<class T>inline T qToBigEndian(T v){return v;}
#else
#error "Qt-free Ostinato requires a compiler with known host byte order"
#endif
template<class T>inline T qFromBigEndian(T v){return qToBigEndian(v);}template<class T>inline T qFromBigEndian(const void*src){T v;std::memcpy(&v,src,sizeof v);return qFromBigEndian(v);}template<class T>inline void qToBigEndian(T v,void*out){T n=qToBigEndian(v);std::memcpy(out,&n,sizeof n);}
