#pragma once

#include <string>
#include <map>
#include <vector>
#include <variant>
#include <memory>

namespace json {

struct Value;
using Array  = std::vector<Value>;
using Object = std::map<std::string, Value>;

struct Value {
    enum Type { TNull, TBool, TNumber, TString, TArray, TObject };
    Type t{ TNull };
    bool b{ false };
    double n{ 0.0 };
    std::string s;
    std::shared_ptr<Array>  a;
    std::shared_ptr<Object> o;

    Value() = default;
    Value(std::nullptr_t)        { t = TNull; }
    Value(bool v)                { t = TBool;   b = v; }
    Value(int v)                 { t = TNumber; n = v; }
    Value(long long v)           { t = TNumber; n = (double)v; }
    Value(double v)              { t = TNumber; n = v; }
    Value(const char* v)         { t = TString; s = v ? v : ""; }
    Value(const std::string& v)  { t = TString; s = v; }
    Value(const Array& v)        { t = TArray;  a = std::make_shared<Array>(v); }
    Value(const Object& v)       { t = TObject; o = std::make_shared<Object>(v); }

    bool isNull() const   { return t == TNull; }
    bool isString() const { return t == TString; }
    bool isNumber() const { return t == TNumber; }
    bool isBool() const   { return t == TBool; }
    bool isObject() const { return t == TObject; }
    bool isArray() const  { return t == TArray; }

    const std::string& asString(const std::string& def = "") const { return t == TString ? s : def; }
    double             asNumber(double def = 0) const              { return t == TNumber ? n : def; }
    bool               asBool(bool def = false) const              { return t == TBool ? b : def; }
    long long          asInt(long long def = 0) const              { return t == TNumber ? (long long)n : def; }
    const Object&      asObject() const                            { static Object empty; return o ? *o : empty; }
    const Array&       asArray() const                             { static Array empty; return a ? *a : empty; }

    const Value& at(const std::string& k) const {
        static Value none;
        if (!isObject() || !o) return none;
        auto it = o->find(k);
        return (it == o->end()) ? none : it->second;
    }
    Value& operator[](const std::string& k) {
        if (t != TObject) { t = TObject; o = std::make_shared<Object>(); }
        return (*o)[k];
    }
};

std::string serialize(const Value& v);
bool        parse(const std::string& src, Value& out);

}
