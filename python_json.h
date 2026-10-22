#pragma once

#include <QByteArray>
#include <QList>
#include <QPair>
#include <QString>
#include <QStringList>

// JSON as Python reads and writes it, which Calibre's files hold: objects
// keep their keys in order, floats keep their ".0", and json.dumps()'s
// spelling, ", " and ": " between items, with or without ensure_ascii.
namespace PythonJson {

struct Value
{
    enum class Kind { Null, Boolean, Integer, Float, String, List, Dict };

    Kind kind = Kind::Null;
    bool boolean = false;
    qint64 integer = 0;
    double real = 0;
    QString string;
    QList<Value> items;
    QList<QPair<QString, Value>> fields;

    static Value null() { return {}; }
    static Value from(bool value);
    static Value from(qint64 value);
    static Value from(int value) { return from(qint64(value)); }
    static Value from(double value);
    static Value from(const QString &value);
    static Value from(const QStringList &values);
    static Value dict();

    // Sets a key of a dict, at its place, or last when new.
    void insert(const QString &key, const Value &value);
    const Value *find(const QString &key) const;
};

// Reads a JSON text, objects keeping their keys' order; empty with `ok`
// false when it is no JSON.
Value parse(const QByteArray &json, bool *ok = nullptr);

struct Format
{
    // Spaces per level; negative writes everything on one line.
    int indent = -1;
    bool sortKeys = false;
    // Everything outside printable ASCII as \u escapes.
    bool ensureAscii = false;
};

// json.dumps().
QString dumps(const Value &value, const Format &format = {});

// Python's float repr: the shortest text that reads back, with ".0" on
// whole numbers; NaN and Infinity as json.dumps() writes them.
QString floatRepr(double value);

} // namespace PythonJson
