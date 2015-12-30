/****************************************************************************
**
** Copyright (C) 2015 The Qt Company Ltd.
** Contact: http://www.qt.io/licensing/
**
** This file is part of the QtQml module of the Qt Toolkit.
**
** $QT_BEGIN_LICENSE:LGPL21$
** Commercial License Usage
** Licensees holding valid commercial Qt licenses may use this file in
** accordance with the commercial license agreement provided with the
** Software or, alternatively, in accordance with the terms contained in
** a written agreement between you and The Qt Company. For licensing terms
** and conditions see http://www.qt.io/terms-conditions. For further
** information use the contact form at http://www.qt.io/contact-us.
**
** GNU Lesser General Public License Usage
** Alternatively, this file may be used under the terms of the GNU Lesser
** General Public License version 2.1 or version 3 as published by the Free
** Software Foundation and appearing in the file LICENSE.LGPLv21 and
** LICENSE.LGPLv3 included in the packaging of this file. Please review the
** following information to ensure the GNU Lesser General Public License
** requirements will be met: https://www.gnu.org/licenses/lgpl.html and
** http://www.gnu.org/licenses/old-licenses/lgpl-2.1.html.
**
** As a special exception, The Qt Company gives you certain additional
** rights. These rights are described in The Qt Company LGPL Exception
** version 1.1, included in the file LGPL_EXCEPTION.txt in this package.
**
** $QT_END_LICENSE$
**
****************************************************************************/

#include "qv4debugjob.h"

#include <private/qv4script_p.h>
#include <private/qqmlcontext_p.h>
#include <private/qv4qobjectwrapper_p.h>

#include <QtQml/qqmlengine.h>

QT_BEGIN_NAMESPACE

QV4DebugJob::~QV4DebugJob()
{
}

JavaScriptJob::JavaScriptJob(QV4::ExecutionEngine *engine, int frameNr,
                                   const QString &script) :
    engine(engine), frameNr(frameNr), script(script), resultIsException(false)
{}

void JavaScriptJob::run()
{
    QV4::Scope scope(engine);

    QV4::ExecutionContextSaver saver(scope);

    QV4::ExecutionContext *ctx = engine->currentContext;
    QObject scopeObject;
    if (frameNr < 0) { // Use QML context if available
        QQmlEngine *qmlEngine = engine->qmlEngine();
        if (qmlEngine) {
            QQmlContext *qmlRootContext = qmlEngine->rootContext();
            QQmlContextPrivate *ctxtPriv = QQmlContextPrivate::get(qmlRootContext);

            QV4::ScopedObject withContext(scope, engine->newObject());
            for (int ii = 0; ii < ctxtPriv->instances.count(); ++ii) {
                QObject *object = ctxtPriv->instances.at(ii);
                if (QQmlContext *context = qmlContext(object)) {
                    if (QQmlContextData *cdata = QQmlContextData::get(context)) {
                        QV4::ScopedValue v(scope, QV4::QObjectWrapper::wrap(engine, object));
                        withContext->put(engine, cdata->findObjectId(object), v);
                    }
                }
            }
            if (!engine->qmlContext()) {
                engine->pushContext(ctx->newQmlContext(QQmlContextData::get(qmlRootContext),
                                                       &scopeObject));
                ctx = engine->currentContext;
            }
            engine->pushContext(ctx->newWithContext(withContext->toObject(engine)));
            ctx = engine->currentContext;
        }
    } else {
        if (frameNr > 0) {
            for (int i = 0; i < frameNr; ++i) {
                ctx = engine->parentContext(ctx);
            }
            engine->pushContext(ctx);
        }
    }

    QV4::Script script(ctx, this->script);
    script.strictMode = ctx->d()->strictMode;
    // In order for property lookups in QML to work, we need to disable fast v4 lookups. That
    // is a side-effect of inheritContext.
    script.inheritContext = true;
    script.parse();
    QV4::ScopedValue result(scope);
    if (!scope.engine->hasException)
        result = script.run();
    if (scope.engine->hasException) {
        result = scope.engine->catchException();
        resultIsException = true;
    }
    handleResult(result);
}

bool JavaScriptJob::hasExeption() const
{
    return resultIsException;
}

ValueLookupJob::ValueLookupJob(const QJsonArray &handles, QV4DataCollector *collector) :
    collector(collector), handles(handles) {}

void ValueLookupJob::run()
{
    // Open a QML context if we don't have one, yet. We might run into QML objects when looking up
    // refs and that will crash without a valid QML context. Mind that engine->qmlContext() is only
    // set if the engine is currently executing QML code.
    QScopedPointer<QObject> scopeObject;
    QV4::ExecutionEngine *engine = collector->engine();
    if (engine->qmlEngine() && !engine->qmlContext()) {
        scopeObject.reset(new QObject);
        engine->pushContext(engine->currentContext->newQmlContext(
                                QQmlContextData::get(engine->qmlEngine()->rootContext()),
                                scopeObject.data()));
    }
    foreach (const QJsonValue &handle, handles) {
        QV4DataCollector::Ref ref = handle.toInt();
        if (!collector->isValidRef(ref)) {
            exception = QString::fromLatin1("Invalid Ref: %1").arg(ref);
            break;
        }
        result[QString::number(ref)] = collector->lookupRef(ref);
    }
    collectedRefs = collector->flushCollectedRefs();
    if (scopeObject)
        engine->popContext();
}

const QString &ValueLookupJob::exceptionMessage() const
{
    return exception;
}

const QJsonObject &ValueLookupJob::returnValue() const
{
    return result;
}

const QJsonArray &ValueLookupJob::refs() const
{
    return collectedRefs;
}

ExpressionEvalJob::ExpressionEvalJob(QV4::ExecutionEngine *engine, int frameNr,
                                     const QString &expression, QV4DataCollector *collector) :
    JavaScriptJob(engine, frameNr, expression), collector(collector)
{
}

void ExpressionEvalJob::handleResult(QV4::ScopedValue &value)
{
    if (hasExeption())
        exception = value->toQStringNoThrow();
    result = collector->lookupRef(collector->collect(value));
    collectedRefs = collector->flushCollectedRefs();
}

const QString &ExpressionEvalJob::exceptionMessage() const
{
    return exception;
}

const QJsonObject &ExpressionEvalJob::returnValue() const
{
    return result;
}

const QJsonArray &ExpressionEvalJob::refs() const
{
    return collectedRefs;
}

GatherSourcesJob::GatherSourcesJob(QV4::ExecutionEngine *engine)
    : engine(engine)
{}

void GatherSourcesJob::run()
{
    foreach (QV4::CompiledData::CompilationUnit *unit, engine->compilationUnits) {
        QString fileName = unit->fileName();
        if (!fileName.isEmpty())
            sources.append(fileName);
    }
}

const QStringList &GatherSourcesJob::result() const
{
    return sources;
}

ArgumentCollectJob::ArgumentCollectJob(QV4::ExecutionEngine *engine, QV4DataCollector *collector,
                                       QStringList *names, int frameNr, int scopeNr)
    : engine(engine)
    , collector(collector)
    , names(names)
    , frameNr(frameNr)
    , scopeNr(scopeNr)
{}

void ArgumentCollectJob::run()
{
    if (frameNr < 0)
        return;

    QV4::Scope scope(engine);
    QV4::Scoped<QV4::CallContext> ctxt(
                scope, QV4DataCollector::findScope(QV4DataCollector::findContext(engine, frameNr), scopeNr));
    if (!ctxt)
        return;

    QV4::ScopedValue v(scope);
    int nFormals = ctxt->formalCount();
    for (unsigned i = 0, ei = nFormals; i != ei; ++i) {
        QString qName;
        if (QV4::Identifier *name = ctxt->formals()[nFormals - i - 1])
            qName = name->string;
        names->append(qName);
        v = ctxt->argument(i);
        collector->collect(v);
    }
}

LocalCollectJob::LocalCollectJob(QV4::ExecutionEngine *engine, QV4DataCollector *collector,
                                 QStringList *names, int frameNr, int scopeNr)
    : engine(engine)
    , collector(collector)
    , names(names)
    , frameNr(frameNr)
    , scopeNr(scopeNr)
{}

void LocalCollectJob::run()
{
    if (frameNr < 0)
        return;

    QV4::Scope scope(engine);
    QV4::Scoped<QV4::CallContext> ctxt(
                scope, QV4DataCollector::findScope(QV4DataCollector::findContext(engine, frameNr), scopeNr));
    if (!ctxt)
        return;

    QV4::ScopedValue v(scope);
    for (unsigned i = 0, ei = ctxt->variableCount(); i != ei; ++i) {
        QString qName;
        if (QV4::Identifier *name = ctxt->variables()[i])
            qName = name->string;
        names->append(qName);
        v = ctxt->d()->locals[i];
        collector->collect(v);
    }
}

ThisCollectJob::ThisCollectJob(QV4::ExecutionEngine *engine, QV4DataCollector *collector,
                               int frameNr)
    : engine(engine)
    , collector(collector)
    , frameNr(frameNr)
    , thisRef(QV4DataCollector::s_invalidRef)
{}

void ThisCollectJob::run()
{
    QV4::Scope scope(engine);
    QV4::ScopedContext ctxt(scope, QV4DataCollector::findContext(engine, frameNr));
    while (ctxt) {
        if (QV4::CallContext *cCtxt = ctxt->asCallContext())
            if (cCtxt->d()->activation)
                break;
        ctxt = ctxt->d()->outer;
    }

    if (!ctxt)
        return;

    QV4::ScopedValue o(scope, ctxt->asCallContext()->d()->activation);
    thisRef = collector->collect(o);
}

QV4DataCollector::Ref ThisCollectJob::foundRef() const
{
    return thisRef;
}

ExceptionCollectJob::ExceptionCollectJob(QV4::ExecutionEngine *engine, QV4DataCollector *collector)
    : engine(engine)
    , collector(collector)
    , exception(QV4DataCollector::s_invalidRef)
{}

void ExceptionCollectJob::run()
{
    QV4::Scope scope(engine);
    QV4::ScopedValue v(scope, *engine->exceptionValue);
    exception = collector->collect(v);
}

QV4DataCollector::Ref ExceptionCollectJob::exceptionValue() const
{
    return exception;
}

EvalJob::EvalJob(QV4::ExecutionEngine *engine, const QString &script) :
    JavaScriptJob(engine, /*frameNr*/-1, script), result(false)
{}

void EvalJob::handleResult(QV4::ScopedValue &result)
{
    this->result = result->toBoolean();
}

bool EvalJob::resultAsBoolean() const
{
    return result;
}

QT_END_NAMESPACE