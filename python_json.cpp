#include "python_json.h"

#include <QLocale>

#include <algorithm>
#include <cmath>

namespace PythonJson {

Value Value::from(bool value)
{
    Value result;
    result.kind = Kind::Boolean;
    result.boolean = value;
    return result;
}

Value Value::from(qint64 value)
{
    Value result;
    result.kind = Kind::Integer;
    result.integer = value;
    return result;
}

Value Value::from(double value)
{
    Value result;
    result.kind = Kind::Float;
    result.real = value;
    return result;
}

Value Value::from(const QString &value)
{
    Value result;
    result.kind = Kind::String;
    result.string = value;
    return result;
}

Value Value::from(const QStringList &values)
{
    Value result;
    result.kind = Kind::List;
    for (const QString &value : values)
        result.items.append(from(value));
    return result;
}

Value Value::dict()
{
    Value result;
    result.kind = Kind::Dict;
    return result;
}

void Value::insert(const QString &key, const Value &value)
{
    for (auto &field : fields) {
        if (field.first == key) {
            field.second = value;
            return;
        }
    }
    fields.append({ key, value });
}

const Value *Value::find(const QString &key) const
{
    for (const auto &field : fields) {
        if (field.first == key)
            return &field.second;
    }
    return nullptr;
}

namespace {

class Reader
{
public:
    explicit Reader(const QString &text) : m_text(text) {}

    bool read(Value *value)
    {
        if (!readValue(value))
            return false;
        skipSpace();
        return m_at == m_text.size();
    }

private:
    void skipSpace()
    {
        while (m_at < m_text.size() && (m_text.at(m_at) == u' ' || m_text.at(m_at) == u'\t'
                                        || m_text.at(m_at) == u'\n' || m_text.at(m_at) == u'\r'))
            ++m_at;
    }

    bool literal(const char *word)
    {
        const QLatin1String text(word);
        if (QStringView(m_text).mid(m_at, text.size()) != text)
            return false;
        m_at += text.size();
        return true;
    }

    bool readString(QString *out)
    {
        if (m_at >= m_text.size() || m_text.at(m_at) != u'"')
            return false;
        ++m_at;
        while (m_at < m_text.size()) {
            const QChar c = m_text.at(m_at++);
            if (c == u'"')
                return true;
            if (c != u'\\') {
                out->append(c);
                continue;
            }
            if (m_at >= m_text.size())
                return false;
            const QChar escape = m_text.at(m_at++);
            switch (escape.unicode()) {
            case u'"': out->append(u'"'); break;
            case u'\\': out->append(u'\\'); break;
            case u'/': out->append(u'/'); break;
            case u'b': out->append(u'\b'); break;
            case u'f': out->append(u'\f'); break;
            case u'n': out->append(u'\n'); break;
            case u'r': out->append(u'\r'); break;
            case u't': out->append(u'\t'); break;
            case u'u': {
                bool ok = false;
                const ushort code = QStringView(m_text).mid(m_at, 4).toUShort(&ok, 16);
                if (!ok)
                    return false;
                m_at += 4;
                out->append(QChar(code));
                break;
            }
            default:
                return false;
            }
        }
        return false;
    }

    bool readValue(Value *value)
    {
        skipSpace();
        if (m_at >= m_text.size())
            return false;
        const QChar c = m_text.at(m_at);
        if (c == u'"') {
            value->kind = Value::Kind::String;
            return readString(&value->string);
        }
        if (c == u'{') {
            ++m_at;
            value->kind = Value::Kind::Dict;
            skipSpace();
            if (m_at < m_text.size() && m_text.at(m_at) == u'}') {
                ++m_at;
                return true;
            }
            while (true) {
                skipSpace();
                QString key;
                Value item;
                if (!readString(&key))
                    return false;
                skipSpace();
                if (m_at >= m_text.size() || m_text.at(m_at++) != u':' || !readValue(&item))
                    return false;
                value->insert(key, item);
                skipSpace();
                if (m_at >= m_text.size())
                    return false;
                const QChar next = m_text.at(m_at++);
                if (next == u'}')
                    return true;
                if (next != u',')
                    return false;
            }
        }
        if (c == u'[') {
            ++m_at;
            value->kind = Value::Kind::List;
            skipSpace();
            if (m_at < m_text.size() && m_text.at(m_at) == u']') {
                ++m_at;
                return true;
            }
            while (true) {
                Value item;
                if (!readValue(&item))
                    return false;
                value->items.append(item);
                skipSpace();
                if (m_at >= m_text.size())
                    return false;
                const QChar next = m_text.at(m_at++);
                if (next == u']')
                    return true;
                if (next != u',')
                    return false;
            }
        }
        if (literal("null")) {
            value->kind = Value::Kind::Null;
            return true;
        }
        if (literal("true") || literal("false")) {
            value->kind = Value::Kind::Boolean;
            value->boolean = m_text.at(m_at - 1) == u'e' && m_text.at(m_at - 2) == u'u';
            return true;
        }
        if (literal("NaN")) {
            value->kind = Value::Kind::Float;
            value->real = std::nan("");
            return true;
        }
        if (literal("Infinity") || literal("-Infinity")) {
            value->kind = Value::Kind::Float;
            value->real = c == u'-' ? -INFINITY : INFINITY;
            return true;
        }
        const qsizetype start = m_at;
        while (m_at < m_text.size() && QStringView(u"+-0123456789.eE").contains(m_text.at(m_at)))
            ++m_at;
        const QStringView number = QStringView(m_text).mid(start, m_at - start);
        bool ok = false;
        if (number.contains(u'.') || number.contains(u'e') || number.contains(u'E')) {
            value->kind = Value::Kind::Float;
            value->real = number.toDouble(&ok);
        } else {
            value->kind = Value::Kind::Integer;
            value->integer = number.toLongLong(&ok);
        }
        return ok;
    }

    const QString m_text;
    qsizetype m_at = 0;
};

QString quoted(const QString &text, bool ensureAscii)
{
    QString result = QStringLiteral("\"");
    for (const QChar character : text) {
        const char16_t code = character.unicode();
        switch (code) {
        case u'"': result += QStringLiteral("\\\""); break;
        case u'\\': result += QStringLiteral("\\\\"); break;
        case u'\n': result += QStringLiteral("\\n"); break;
        case u'\r': result += QStringLiteral("\\r"); break;
        case u'\t': result += QStringLiteral("\\t"); break;
        case u'\b': result += QStringLiteral("\\b"); break;
        case u'\f': result += QStringLiteral("\\f"); break;
        default:
            if (code < 0x20 || (ensureAscii && code > 0x7e))
                result += QStringLiteral("\\u%1").arg(int(code), 4, 16, QLatin1Char('0'));
            else
                result += character;
        }
    }
    return result + u'"';
}

void write(QString &out, const Value &value, const Format &format, int depth)
{
    const bool lines = format.indent >= 0;
    const QString indent = lines ? u'\n' + QString(format.indent * (depth + 1), u' ') : QString();
    const QString closing = lines ? u'\n' + QString(format.indent * depth, u' ') : QString();
    const QString separator = lines ? QStringLiteral(",") : QStringLiteral(", ");
    switch (value.kind) {
    case Value::Kind::Null:
        out += QStringLiteral("null");
        return;
    case Value::Kind::Boolean:
        out += value.boolean ? QStringLiteral("true") : QStringLiteral("false");
        return;
    case Value::Kind::Integer:
        out += QString::number(value.integer);
        return;
    case Value::Kind::Float:
        out += floatRepr(value.real);
        return;
    case Value::Kind::String:
        out += quoted(value.string, format.ensureAscii);
        return;
    case Value::Kind::List:
        if (value.items.isEmpty()) {
            out += QStringLiteral("[]");
            return;
        }
        out += u'[';
        for (qsizetype index = 0; index < value.items.size(); ++index) {
            out += (index ? separator : QString()) + indent;
            write(out, value.items.at(index), format, depth + 1);
        }
        out += closing + u']';
        return;
    case Value::Kind::Dict: {
        if (value.fields.isEmpty()) {
            out += QStringLiteral("{}");
            return;
        }
        QList<QPair<QString, Value>> fields = value.fields;
        if (format.sortKeys)
            std::sort(fields.begin(), fields.end(), [](const auto &a, const auto &b) { return a.first < b.first; });
        out += u'{';
        for (qsizetype index = 0; index < fields.size(); ++index) {
            out += (index ? separator : QString()) + indent + quoted(fields.at(index).first, format.ensureAscii)
                + QStringLiteral(": ");
            write(out, fields.at(index).second, format, depth + 1);
        }
        out += closing + u'}';
        return;
    }
    }
}

} // namespace

Value parse(const QByteArray &json, bool *ok)
{
    Value value;
    const bool read = Reader(QString::fromUtf8(json)).read(&value);
    if (ok)
        *ok = read;
    return read ? value : Value();
}

QString dumps(const Value &value, const Format &format)
{
    QString out;
    write(out, value, format, 0);
    return out;
}

QString floatRepr(double value)
{
    if (std::isnan(value))
        return QStringLiteral("NaN");
    if (std::isinf(value))
        return value > 0 ? QStringLiteral("Infinity") : QStringLiteral("-Infinity");
    QString text = QString::number(value, 'g', QLocale::FloatingPointShortest);
    if (!text.contains(u'.') && !text.contains(u'e') && !text.contains(u'n'))
        text += QStringLiteral(".0");
    return text;
}

} // namespace PythonJson
