/*
Copyright (C) 2010, 2014 Srivats P.

This file is part of "Ostinato"

This is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program.  If not, see <http://www.gnu.org/licenses/>
*/

#include "userscript.h"

#include <quickjs.h>

#include <QRegExp>

class UserScriptEngine
{
public:
    UserScriptEngine()
        : runtime(JS_NewRuntime()), context(nullptr), protocol(JS_UNDEFINED)
    {
    }

    ~UserScriptEngine()
    {
        if (context) {
            JS_FreeValue(context, protocol);
            JS_FreeContext(context);
        }
        if (runtime)
            JS_FreeRuntime(runtime);
    }

    bool resetContext()
    {
        if (context) {
            JS_FreeValue(context, protocol);
            JS_FreeContext(context);
        }
        context = runtime ? JS_NewContext(runtime) : nullptr;
        protocol = JS_UNDEFINED;
        return context;
    }

    JSRuntime *runtime;
    JSContext *context;
    JSValue protocol;
};

namespace {

enum HostProperty {
    HostName,
    HostFrameSizeVariable,
    HostFrameVariableCount
};

enum HostMethod {
    HostReset,
    HostSetName,
    HostIsFrameSizeVariable,
    HostSetFrameSizeVariable,
    HostSetFrameVariableCount,
    HostPayloadProtocolId,
    HostFrameOffset,
    HostFramePayloadSize,
    HostIsPayloadValueVariable,
    HostIsPayloadSizeVariable,
    HostPayloadVariableCount,
    HostFrameHeaderCksum,
    HostFramePayloadCksum
};

int argumentInt(JSContext *context, int argc, JSValueConst *argv,
                int index, int defaultValue, bool *ok = nullptr)
{
    int32_t value = defaultValue;
    const bool converted = index >= argc
        || JS_ToInt32(context, &value, argv[index]) == 0;
    if (ok)
        *ok = converted;
    return value;
}

JSValue hostPropertyGetter(JSContext *context, JSValueConst, int magic)
{
    UserProtocol *protocol = static_cast<UserProtocol *>(
        JS_GetContextOpaque(context));
    switch (magic) {
    case HostName:
        return JS_NewString(context, protocol->name().toUtf8().constData());
    case HostFrameSizeVariable:
        return JS_NewBool(context, protocol->isProtocolFrameSizeVariable());
    case HostFrameVariableCount:
        return JS_NewInt32(context, protocol->protocolFrameVariableCount());
    default:
        return JS_UNDEFINED;
    }
}

JSValue hostPropertySetter(JSContext *context, JSValueConst, JSValueConst value,
                           int magic)
{
    UserProtocol *protocol = static_cast<UserProtocol *>(
        JS_GetContextOpaque(context));
    if (magic == HostName) {
        const char *text = JS_ToCString(context, value);
        if (!text)
            return JS_EXCEPTION;
        QString name = QString::fromUtf8(text);
        JS_FreeCString(context, text);
        protocol->setName(name);
        return JS_UNDEFINED;
    }

    if (magic == HostFrameSizeVariable) {
        const int boolean = JS_ToBool(context, value);
        if (boolean < 0)
            return JS_EXCEPTION;
        protocol->setProtocolFrameSizeVariable(boolean);
    } else {
        int32_t number;
        if (JS_ToInt32(context, &number, value) < 0)
            return JS_EXCEPTION;
        protocol->setProtocolFrameVariableCount(number);
    }
    return JS_UNDEFINED;
}

JSValue hostMethod(JSContext *context, JSValueConst, int argc,
                   JSValueConst *argv, int magic)
{
    UserProtocol *protocol = static_cast<UserProtocol *>(
        JS_GetContextOpaque(context));
    if (magic == HostReset) {
        protocol->reset();
        return JS_UNDEFINED;
    }
    if (magic == HostSetName) {
        const char *text = JS_ToCString(
            context, argc > 0 ? argv[0] : JS_UNDEFINED);
        if (!text)
            return JS_EXCEPTION;
        QString name = QString::fromUtf8(text);
        JS_FreeCString(context, text);
        protocol->setName(name);
        return JS_UNDEFINED;
    }
    if (magic == HostIsFrameSizeVariable)
        return JS_NewBool(context, protocol->isProtocolFrameSizeVariable());
    if (magic == HostSetFrameSizeVariable) {
        const int value = JS_ToBool(
            context, argc > 0 ? argv[0] : JS_UNDEFINED);
        if (value < 0)
            return JS_EXCEPTION;
        protocol->setProtocolFrameSizeVariable(value);
        return JS_UNDEFINED;
    }
    if (magic == HostSetFrameVariableCount) {
        bool converted;
        const int value = argumentInt(context, argc, argv, 0, 0, &converted);
        if (!converted)
            return JS_EXCEPTION;
        protocol->setProtocolFrameVariableCount(value);
        return JS_UNDEFINED;
    }

    bool ok = true;
    const int first = argumentInt(context, argc, argv, 0, 0, &ok);
    if (!ok)
        return JS_EXCEPTION;

    switch (magic) {
    case HostPayloadProtocolId:
        return JS_NewUint32(context, protocol->payloadProtocolId(
            static_cast<UserProtocol::ProtocolIdType>(first)));
    case HostFrameOffset:
        return JS_NewInt32(context, protocol->protocolFrameOffset(first));
    case HostFramePayloadSize:
        return JS_NewInt32(context, protocol->protocolFramePayloadSize(first));
    case HostIsPayloadValueVariable:
        return JS_NewBool(context,
                          protocol->isProtocolFramePayloadValueVariable());
    case HostIsPayloadSizeVariable:
        return JS_NewBool(context,
                          protocol->isProtocolFramePayloadSizeVariable());
    case HostPayloadVariableCount:
        return JS_NewInt32(context,
                           protocol->protocolFramePayloadVariableCount());
    case HostFrameHeaderCksum:
    case HostFramePayloadCksum: {
        const int checksumType = argumentInt(
            context, argc, argv, 1, AbstractProtocol::CksumIp, &ok);
        if (!ok)
            return JS_EXCEPTION;
        const AbstractProtocol::CksumType type =
            static_cast<AbstractProtocol::CksumType>(checksumType);
        const quint32 checksum = magic == HostFrameHeaderCksum
            ? protocol->protocolFrameHeaderCksum(first, type)
            : protocol->protocolFramePayloadCksum(first, type);
        return JS_NewUint32(context, checksum);
    }
    default:
        return JS_UNDEFINED;
    }
}

bool defineHostProperty(JSContext *context, JSValueConst object,
                        const char *name, int magic)
{
    const JSAtom atom = JS_NewAtom(context, name);
    const int result = JS_DefinePropertyGetSet(
        context, object, atom,
        JS_NewCFunctionMagic(context, (JSCFunctionMagic *)hostPropertyGetter,
                             name, 0, JS_CFUNC_getter_magic, magic),
        JS_NewCFunctionMagic(context, (JSCFunctionMagic *)hostPropertySetter,
                             name, 1, JS_CFUNC_setter_magic, magic),
        JS_PROP_CONFIGURABLE | JS_PROP_ENUMERABLE);
    JS_FreeAtom(context, atom);
    return result >= 0;
}

bool defineHostMethod(JSContext *context, JSValueConst object,
                      const char *name, int length, int magic)
{
    return JS_SetPropertyStr(
        context, object, name,
        JS_NewCFunctionMagic(context, (JSCFunctionMagic *)hostMethod,
                             name, length, JS_CFUNC_generic_magic, magic)) >= 0;
}

bool defineConstant(JSContext *context, JSValueConst object,
                    const char *name, int value)
{
    return JS_SetPropertyStr(
        context, object, name, JS_NewInt32(context, value)) >= 0;
}

bool installHostApi(UserScriptEngine *engine, UserProtocol *userProtocol)
{
    JSContext *context = engine->context;
    JS_SetContextOpaque(context, userProtocol);
    engine->protocol = JS_NewObject(context);
    if (JS_IsException(engine->protocol))
        return false;

    if (!defineHostProperty(context, engine->protocol, "name", HostName)
            || !defineHostProperty(context, engine->protocol,
                                   "protocolFrameSizeVariable",
                                   HostFrameSizeVariable)
            || !defineHostProperty(context, engine->protocol,
                                   "protocolFrameVariableCount",
                                   HostFrameVariableCount)
            || !defineHostMethod(context, engine->protocol, "reset", 0,
                                 HostReset)
            || !defineHostMethod(context, engine->protocol, "setName", 1,
                                 HostSetName)
            || !defineHostMethod(context, engine->protocol,
                                 "isProtocolFrameSizeVariable", 0,
                                 HostIsFrameSizeVariable)
            || !defineHostMethod(context, engine->protocol,
                                 "setProtocolFrameSizeVariable", 1,
                                 HostSetFrameSizeVariable)
            || !defineHostMethod(context, engine->protocol,
                                 "setProtocolFrameVariableCount", 1,
                                 HostSetFrameVariableCount)
            || !defineHostMethod(context, engine->protocol,
                                 "payloadProtocolId", 1,
                                 HostPayloadProtocolId)
            || !defineHostMethod(context, engine->protocol,
                                 "protocolFrameOffset", 1, HostFrameOffset)
            || !defineHostMethod(context, engine->protocol,
                                 "protocolFramePayloadSize", 1,
                                 HostFramePayloadSize)
            || !defineHostMethod(context, engine->protocol,
                                 "isProtocolFramePayloadValueVariable", 0,
                                 HostIsPayloadValueVariable)
            || !defineHostMethod(context, engine->protocol,
                                 "isProtocolFramePayloadSizeVariable", 0,
                                 HostIsPayloadSizeVariable)
            || !defineHostMethod(context, engine->protocol,
                                 "protocolFramePayloadVariableCount", 0,
                                 HostPayloadVariableCount)
            || !defineHostMethod(context, engine->protocol,
                                 "protocolFrameHeaderCksum", 2,
                                 HostFrameHeaderCksum)
            || !defineHostMethod(context, engine->protocol,
                                 "protocolFramePayloadCksum", 2,
                                 HostFramePayloadCksum))
        return false;

    JSValue global = JS_GetGlobalObject(context);
    if (JS_IsException(global))
        return false;
    if (JS_SetPropertyStr(context, global, "protocol",
                          JS_DupValue(context, engine->protocol)) < 0) {
        JS_FreeValue(context, global);
        return false;
    }

    JSValue meta = JS_NewObject(context);
    if (JS_IsException(meta)
            || !defineConstant(context, meta, "ProtocolIdLlc",
                               UserProtocol::ProtocolIdLlc)
            || !defineConstant(context, meta, "ProtocolIdEth",
                               UserProtocol::ProtocolIdEth)
            || !defineConstant(context, meta, "ProtocolIdIp",
                               UserProtocol::ProtocolIdIp)
            || !defineConstant(context, meta, "ProtocolIdTcpUdp",
                               UserProtocol::ProtocolIdTcpUdp)
            || !defineConstant(context, meta, "CksumIp",
                               UserProtocol::CksumIp)
            || !defineConstant(context, meta, "CksumIpPseudo",
                               UserProtocol::CksumIpPseudo)
            || !defineConstant(context, meta, "CksumTcpUdp",
                               UserProtocol::CksumTcpUdp)
            || !defineConstant(context, meta, "IncludeCksumField",
                               UserProtocol::IncludeCksumField)) {
        if (!JS_IsException(meta))
            JS_FreeValue(context, meta);
        JS_FreeValue(context, global);
        return false;
    }
    if (JS_SetPropertyStr(context, global, "Protocol", meta) < 0) {
        JS_FreeValue(context, global);
        return false;
    }
    JS_FreeValue(context, global);
    return true;
}

void takeException(JSContext *context, int fallbackLine,
                   int &lineNumber, QString &text)
{
    JSValue exception = JS_GetException(context);
    const char *message = JS_ToCString(context, exception);
    text = message ? QString::fromUtf8(message) : QString("JavaScript exception");
    if (message) {
        JS_FreeCString(context, message);
    } else {
        JSValue secondary = JS_GetException(context);
        JS_FreeValue(context, secondary);
    }

    lineNumber = fallbackLine;
    JSValue stackValue = JS_GetPropertyStr(context, exception, "stack");
    if (!JS_IsException(stackValue)) {
        const char *stack = JS_ToCString(context, stackValue);
        if (stack) {
            QRegExp location("userscript:(\\d+)");
            if (location.indexIn(QString::fromUtf8(stack)) >= 0)
                lineNumber = location.cap(1).toInt();
            JS_FreeCString(context, stack);
        } else {
            JSValue secondary = JS_GetException(context);
            JS_FreeValue(context, secondary);
        }
    } else {
        JSValue secondary = JS_GetException(context);
        JS_FreeValue(context, secondary);
    }
    JS_FreeValue(context, stackValue);
    JS_FreeValue(context, exception);
}

struct UserFunctionCall
{
    JSValue value;
    bool callable;
};

UserFunctionCall callUserFunction(UserScriptEngine *engine, const char *name,
                                  int argc = 0,
                                  JSValueConst *argv = nullptr)
{
    JSContext *context = engine->context;
    JSValue function = JS_GetPropertyStr(context, engine->protocol, name);
    if (JS_IsException(function))
        return {function, true};
    if (!JS_IsFunction(context, function)) {
        JS_FreeValue(context, function);
        return {JS_UNDEFINED, false};
    }
    JSValue result = JS_Call(context, function, JS_UNDEFINED, argc, argv);
    JS_FreeValue(context, function);
    return {result, true};
}

int32_t userValueToInt32(JSContext *context, JSValue value)
{
    if (JS_IsException(value))
        value = JS_GetException(context);

    int32_t result = 0;
    const int converted = JS_ToInt32(context, &result, value);
    JS_FreeValue(context, value);
    if (converted < 0) {
        JSValue exception = JS_GetException(context);
        JS_FreeValue(context, exception);
    }
    return result;
}

uint32_t userValueToUint32(JSContext *context, JSValue value)
{
    if (JS_IsException(value))
        value = JS_GetException(context);

    uint32_t result = 0;
    const int converted = JS_ToUint32(context, &result, value);
    JS_FreeValue(context, value);
    if (converted < 0) {
        JSValue exception = JS_GetException(context);
        JS_FreeValue(context, exception);
    }
    return result;
}

} // namespace

//
// -------------------- UserScriptProtocol --------------------
//

UserScriptProtocol::UserScriptProtocol(StreamBase *stream, AbstractProtocol *parent)
    : AbstractProtocol(stream, parent),
        engine_(new UserScriptEngine),
        userProtocol_(this)
{
    isScriptValid_ = false;
    errorLineNumber_ = 0;
}

UserScriptProtocol::~UserScriptProtocol()
{
    delete engine_;
}

AbstractProtocol* UserScriptProtocol::createInstance(StreamBase *stream,
    AbstractProtocol *parent)
{
    return new UserScriptProtocol(stream, parent);
}

quint32 UserScriptProtocol::protocolNumber() const
{
    return OstProto::Protocol::kUserScriptFieldNumber;
}

void UserScriptProtocol::protoDataCopyInto(OstProto::Protocol &protocol) const
{
    protocol.MutableExtension(OstProto::userScript)->CopyFrom(data);
    protocol.mutable_protocol_id()->set_id(protocolNumber());
}

void UserScriptProtocol::protoDataCopyFrom(const OstProto::Protocol &protocol)
{
    if (protocol.protocol_id().id() == protocolNumber() &&
            protocol.HasExtension(OstProto::userScript))
        data.MergeFrom(protocol.GetExtension(OstProto::userScript));

    evaluateUserScript();
}

QString UserScriptProtocol::name() const
{
    return QString("%1:{UserScript} [EXPERIMENTAL]").arg(userProtocol_.name());
}

QString UserScriptProtocol::shortName() const
{
    return QString("%1:{Script} [EXPERIMENTAL]").arg(userProtocol_.name());
}

quint32 UserScriptProtocol::protocolId(ProtocolIdType type) const
{
    if (!isScriptValid_)
        return AbstractProtocol::protocolId(type);

    JSValue argument = JS_NewInt32(engine_->context, type);
    UserFunctionCall call = callUserFunction(
        engine_, "protocolId", 1, &argument);
    JS_FreeValue(engine_->context, argument);
    if (!call.callable)
        return AbstractProtocol::protocolId(type);
    return userValueToUint32(engine_->context, call.value);
}

int UserScriptProtocol::fieldCount() const
{
    return userScript_fieldCount;
}

QVariant UserScriptProtocol::fieldData(int index, FieldAttrib attrib,
        int streamIndex) const
{
    switch (index)
    {
    case userScript_program:

        switch(attrib)
        {
        case FieldName:            
            return QString("UserProtocol");

        case FieldValue:
        case FieldTextValue:
            return QString().fromStdString(data.program());

        case FieldFrameValue:
        {
            if (!isScriptValid_)
                return QByteArray();

            JSContext *context = engine_->context;
            JSValue argument = JS_NewInt32(context, streamIndex);
            UserFunctionCall call = callUserFunction(
                engine_, "protocolFrameValue", 1, &argument);
            JS_FreeValue(context, argument);
            JSValue userValue = call.value;
            if (JS_IsException(userValue)) {
                JSValue exception = JS_GetException(context);
                JS_FreeValue(context, exception);
                return QByteArray();
            }
            QByteArray fv;
            const int isArray = JS_IsArray(userValue);
            if (isArray < 0) {
                JS_FreeValue(context, userValue);
                takeException(context, userScriptLineCount(),
                              errorLineNumber_, errorText_);
                isScriptValid_ = false;
                return QByteArray();
            }
            JSValue lengthValue = JS_GetPropertyStr(context, userValue, "length");
            if (JS_IsException(lengthValue)) {
                JS_FreeValue(context, userValue);
                takeException(context, userScriptLineCount(),
                              errorLineNumber_, errorText_);
                isScriptValid_ = false;
                return QByteArray();
            }
            uint32_t length = 0;
            const int lengthConverted = JS_ToUint32(
                context, &length, lengthValue);
            JS_FreeValue(context, lengthValue);
            if (lengthConverted < 0) {
                JS_FreeValue(context, userValue);
                takeException(context, userScriptLineCount(),
                              errorLineNumber_, errorText_);
                isScriptValid_ = false;
                return QByteArray();
            }
            if (isArray) {
                fv.resize(int(length));
                for (uint32_t i = 0; i < length; ++i) {
                    JSValue element = JS_GetPropertyUint32(context, userValue, i);
                    if (JS_IsException(element)) {
                        JS_FreeValue(context, userValue);
                        takeException(context, userScriptLineCount(),
                                      errorLineNumber_, errorText_);
                        isScriptValid_ = false;
                        return QByteArray();
                    }
                    int32_t byte = 0;
                    if (JS_ToInt32(context, &byte, element) < 0) {
                        JS_FreeValue(context, element);
                        JS_FreeValue(context, userValue);
                        takeException(context, userScriptLineCount(),
                                      errorLineNumber_, errorText_);
                        isScriptValid_ = false;
                        return QByteArray();
                    }
                    fv[int(i)] = char(byte & 0xff);
                    JS_FreeValue(context, element);
                }
            }
            JS_FreeValue(context, userValue);

            return fv;
        }
        default:
            break;
        }
        break;

    default:
        qFatal("%s: unimplemented case %d in switch", __PRETTY_FUNCTION__,
            index);
        break;
    }

    return AbstractProtocol::fieldData(index, attrib, streamIndex);
}

bool UserScriptProtocol::setFieldData(int index, const QVariant &value, 
        FieldAttrib attrib)
{
    bool isOk = false;

    if (attrib != FieldValue)
        goto _exit;

    switch (index)
    {
        case userScript_program:
        {
            data.set_program(value.toString().toStdString());
            evaluateUserScript();
            break;
        }
        default:
            qFatal("%s: unimplemented case %d in switch", __PRETTY_FUNCTION__,
                index);
            break;
    }

_exit:
    return isOk;
}

int UserScriptProtocol::protocolFrameSize(int streamIndex) const
{
    if (!isScriptValid_)
        return 0;

    JSContext *context = engine_->context;
    JSValue argument = JS_NewInt32(context, streamIndex);
    UserFunctionCall call = callUserFunction(
        engine_, "protocolFrameSize", 1, &argument);
    JS_FreeValue(context, argument);
    return userValueToInt32(context, call.value);
}

bool UserScriptProtocol::isProtocolFrameSizeVariable() const
{
    return userProtocol_.isProtocolFrameSizeVariable();
}

int UserScriptProtocol::protocolFrameVariableCount() const
{
    return AbstractProtocol::lcm(
            AbstractProtocol::protocolFrameVariableCount(),
            userProtocol_.protocolFrameVariableCount());
}

quint32 UserScriptProtocol::protocolFrameCksum(int streamIndex,
        CksumType cksumType, CksumFlags cksumFlags) const
{
    if (!isScriptValid_)
        return AbstractProtocol::protocolFrameCksum(
            streamIndex, cksumType, cksumFlags);

    JSContext *context = engine_->context;
    JSValue arguments[] = {
        JS_NewInt32(context, streamIndex),
        JS_NewInt32(context, cksumType),
        JS_NewInt32(context, int(cksumFlags))
    };
    UserFunctionCall call = callUserFunction(
        engine_, "protocolFrameCksum", 3, arguments);
    for (int i = 0; i < 3; ++i)
        JS_FreeValue(context, arguments[i]);
    if (!call.callable)
        return AbstractProtocol::protocolFrameCksum(
            streamIndex, cksumType, cksumFlags);
    return userValueToUint32(context, call.value);
}

void UserScriptProtocol::evaluateUserScript() const
{
    isScriptValid_ = false;
    errorLineNumber_ = userScriptLineCount();

    userProtocol_.reset();
    if (!engine_->resetContext()) {
        errorText_ = QString("Unable to create QuickJS context");
        return;
    }
    if (!installHostApi(engine_, &userProtocol_)) {
        takeException(engine_->context, userScriptLineCount(),
                      errorLineNumber_, errorText_);
        userProtocol_.reset();
        return;
    }

    const QByteArray program = fieldData(
        userScript_program, FieldValue).toString().toUtf8();
    JSValue evaluation = JS_Eval(engine_->context, program.constData(),
                                 size_t(program.size()), "userscript",
                                 JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(evaluation)) {
        takeException(engine_->context, userScriptLineCount(),
                      errorLineNumber_, errorText_);
        userProtocol_.reset();
        return;
    }
    JS_FreeValue(engine_->context, evaluation);

    struct FunctionRequirement {
        const char *name;
        bool required;
        bool arrayResult;
    } requirements[] = {
        {"protocolFrameValue", true, true},
        {"protocolFrameSize", true, false},
        {"protocolFrameCksum", false, false},
        {"protocolId", false, false}
    };

    for (const FunctionRequirement &requirement : requirements) {
        const JSAtom property = JS_NewAtom(
            engine_->context, requirement.name);
        const int hasProperty = JS_HasProperty(
            engine_->context, engine_->protocol, property);
        JS_FreeAtom(engine_->context, property);
        if (hasProperty < 0) {
            takeException(engine_->context, userScriptLineCount(),
                          errorLineNumber_, errorText_);
            userProtocol_.reset();
            return;
        }
        if (!hasProperty) {
            if (requirement.required) {
                errorText_ = QString::fromLatin1(requirement.name) + " not set";
                userProtocol_.reset();
                return;
            }
            continue;
        }
        JSValue function = JS_GetPropertyStr(
            engine_->context, engine_->protocol, requirement.name);
        if (JS_IsException(function)) {
            takeException(engine_->context, userScriptLineCount(),
                          errorLineNumber_, errorText_);
            userProtocol_.reset();
            return;
        }
        if (!JS_IsFunction(engine_->context, function)) {
            JS_FreeValue(engine_->context, function);
            errorText_ = QString::fromLatin1(requirement.name)
                       + " is not a function";
            userProtocol_.reset();
            return;
        }
        JSValue value = JS_Call(engine_->context, function, JS_UNDEFINED, 0,
                                nullptr);
        JS_FreeValue(engine_->context, function);
        if (JS_IsException(value)) {
            takeException(engine_->context, userScriptLineCount(),
                          errorLineNumber_, errorText_);
            userProtocol_.reset();
            return;
        }
        const int isArray = requirement.arrayResult
            ? JS_IsArray(value) : 0;
        if (isArray < 0) {
            JS_FreeValue(engine_->context, value);
            takeException(engine_->context, userScriptLineCount(),
                          errorLineNumber_, errorText_);
            userProtocol_.reset();
            return;
        }
        const bool correctType = requirement.arrayResult
            ? isArray : JS_IsNumber(value);
        JS_FreeValue(engine_->context, value);
        if (!correctType) {
            errorText_ = QString::fromLatin1(requirement.name)
                       + (requirement.arrayResult
                              ? " does not return an array"
                              : " does not return a number");
            userProtocol_.reset();
            return;
        }
    }

    errorText_.clear();
    isScriptValid_ = true;
}

bool UserScriptProtocol::isScriptValid() const
{
    return isScriptValid_;
}

int UserScriptProtocol::userScriptErrorLineNumber() const
{
    return errorLineNumber_;
}

QString UserScriptProtocol::userScriptErrorText() const
{
    return errorText_;
}

int UserScriptProtocol::userScriptLineCount() const
{
    return fieldData(userScript_program, FieldValue).toString().count(
            QChar('\n')) + 1;
}

//
// -------------------- UserProtocol --------------------
//

UserProtocol::UserProtocol(AbstractProtocol *parent)
    : parent_ (parent)
{
    reset();
}

void UserProtocol::reset()
{
    name_ = QString();
    protocolFrameSizeVariable_ = false;
    protocolFrameVariableCount_ = 1;
}

QString UserProtocol::name() const
{
    return name_;
}

void UserProtocol::setName(QString &name)
{
    name_ = name;
}

bool UserProtocol::isProtocolFrameSizeVariable() const
{
    return protocolFrameSizeVariable_;
}

void UserProtocol::setProtocolFrameSizeVariable(bool variable)
{
    protocolFrameSizeVariable_ = variable;
}

int UserProtocol::protocolFrameVariableCount() const
{
    return protocolFrameVariableCount_;
}

void UserProtocol::setProtocolFrameVariableCount(int count)
{
    protocolFrameVariableCount_ = count;
}

quint32 UserProtocol::payloadProtocolId(UserProtocol::ProtocolIdType type) const
{
    return parent_->payloadProtocolId(
            static_cast<AbstractProtocol::ProtocolIdType>(type));
}

int UserProtocol::protocolFrameOffset(int streamIndex) const
{
    return parent_->protocolFrameOffset(streamIndex);
}

int UserProtocol::protocolFramePayloadSize(int streamIndex) const
{
    return parent_->protocolFramePayloadSize(streamIndex);
}

bool UserProtocol::isProtocolFramePayloadValueVariable() const
{
    return parent_->isProtocolFramePayloadValueVariable();
}

bool UserProtocol::isProtocolFramePayloadSizeVariable() const
{
    return parent_->isProtocolFramePayloadSizeVariable();
}

int UserProtocol::protocolFramePayloadVariableCount() const
{
    return parent_->protocolFramePayloadVariableCount();
}

quint32 UserProtocol::protocolFrameHeaderCksum(int streamIndex,
    AbstractProtocol::CksumType cksumType) const
{
    return parent_->protocolFrameHeaderCksum(streamIndex, cksumType);
}

quint32 UserProtocol::protocolFramePayloadCksum(int streamIndex,
    AbstractProtocol::CksumType cksumType) const
{
    quint32 cksum;

    cksum = parent_->protocolFramePayloadCksum(streamIndex, cksumType);
    qDebug("UserProto:%s = %d", __FUNCTION__, cksum);
    return cksum;
}

/* vim: set shiftwidth=4 tabstop=8 softtabstop=4 expandtab: */
