#include "json.h"

#if QT_VERSION < QT_VERSION_CHECK(5, 0, 0)

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace wa {
namespace json {

NodePtr detach(const NodePtr &node) {
    if (node && node.use_count() == 1) return node;
    NodePtr copy(new Node);
    if (node) *copy = *node;
    return copy;
}

namespace {

// ── Parser ───────────────────────────────────────────────────────────────
class Parser {
public:
    Parser(const QByteArray &text) : s(text.constData()), n(text.size()), i(0) {}

    NodePtr parse(QString &error) {
        NodePtr root = parseValue(error);
        if (!error.isEmpty()) return NodePtr();
        skipSpace();
        if (i != n) error = "trailing characters after the JSON value";
        return error.isEmpty() ? root : NodePtr();
    }

private:
    void skipSpace() {
        while (i < n) {
            const char c = s[i];
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') { i++; continue; }
            break;
        }
    }

    NodePtr parseValue(QString &error) {
        skipSpace();
        if (i >= n) { error = "unexpected end of JSON"; return NodePtr(); }
        const char c = s[i];
        if (c == '{') return parseObject(error);
        if (c == '[') return parseArray(error);
        if (c == '"') {
            const QString text = parseString(error);
            if (!error.isEmpty()) return NodePtr();
            NodePtr node = makeNode(Node::String);
            node->text = text;
            return node;
        }
        if (c == 't' || c == 'f') return parseBool(error);
        if (c == 'n') return parseNull(error);
        return parseNumber(error);
    }

    NodePtr parseObject(QString &error) {
        NodePtr node = makeNode(Node::Object);
        i++;   // {
        skipSpace();
        if (i < n && s[i] == '}') { i++; return node; }
        while (i < n) {
            skipSpace();
            if (i >= n || s[i] != '"') { error = "object key must be a string"; return NodePtr(); }
            const QString key = parseString(error);
            if (!error.isEmpty()) return NodePtr();
            skipSpace();
            if (i >= n || s[i] != ':') { error = "expected ':' after object key"; return NodePtr(); }
            i++;
            NodePtr value = parseValue(error);
            if (!error.isEmpty()) return NodePtr();
            node->fields.push_back(std::make_pair(key, value));
            skipSpace();
            if (i < n && s[i] == ',') { i++; continue; }
            if (i < n && s[i] == '}') { i++; return node; }
            error = "expected ',' or '}' in object";
            return NodePtr();
        }
        error = "unterminated object";
        return NodePtr();
    }

    NodePtr parseArray(QString &error) {
        NodePtr node = makeNode(Node::Array);
        i++;   // [
        skipSpace();
        if (i < n && s[i] == ']') { i++; return node; }
        while (i < n) {
            NodePtr value = parseValue(error);
            if (!error.isEmpty()) return NodePtr();
            node->items.push_back(value);
            skipSpace();
            if (i < n && s[i] == ',') { i++; continue; }
            if (i < n && s[i] == ']') { i++; return node; }
            error = "expected ',' or ']' in array";
            return NodePtr();
        }
        error = "unterminated array";
        return NodePtr();
    }

    QString parseString(QString &error) {
        QString out;
        i++;   // opening quote
        while (i < n) {
            const char c = s[i];
            if (c == '"') { i++; return out; }
            if (c != '\\') {
                // Copy raw UTF-8 bytes as Latin-1 chars, then fix up: WhatsApp
                // payloads are mostly ASCII, but decode multi-byte sequences.
                if ((c & 0x80) == 0) { out += QChar(uchar(c)); i++; continue; }
                unsigned int cp = 0;
                int extra = 0;
                if ((c & 0xe0) == 0xc0) { cp = uchar(c) & 0x1f; extra = 1; }
                else if ((c & 0xf0) == 0xe0) { cp = uchar(c) & 0x0f; extra = 2; }
                else if ((c & 0xf8) == 0xf0) { cp = uchar(c) & 0x07; extra = 3; }
                else { error = "invalid UTF-8 in JSON string"; return QString(); }
                i++;
                for (int k = 0; k < extra; k++, i++) {
                    if (i >= n || (s[i] & 0xc0) != 0x80) {
                        error = "truncated UTF-8 in JSON string";
                        return QString();
                    }
                    cp = (cp << 6) | (uchar(s[i]) & 0x3f);
                }
                if (cp <= 0xffff) out += QChar(ushort(cp));
                else {
                    const unsigned int v = cp - 0x10000;
                    out += QChar(ushort(0xd800 + (v >> 10)));
                    out += QChar(ushort(0xdc00 + (v & 0x3ff)));
                }
                continue;
            }
            i++;   // backslash
            if (i >= n) break;
            const char e = s[i++];
            switch (e) {
            case '"': out += QChar('"'); break;
            case '\\': out += QChar('\\'); break;
            case '/': out += QChar('/'); break;
            case 'b': out += QChar('\b'); break;
            case 'f': out += QChar('\f'); break;
            case 'n': out += QChar('\n'); break;
            case 'r': out += QChar('\r'); break;
            case 't': out += QChar('\t'); break;
            case 'u': {
                if (i + 4 > n) { error = "truncated \\u escape"; return QString(); }
                unsigned int cp = 0;
                for (int k = 0; k < 4; k++) {
                    const char h = s[i + k];
                    cp <<= 4;
                    if (h >= '0' && h <= '9') cp |= unsigned(h - '0');
                    else if (h >= 'a' && h <= 'f') cp |= unsigned(h - 'a' + 10);
                    else if (h >= 'A' && h <= 'F') cp |= unsigned(h - 'A' + 10);
                    else { error = "bad \\u escape"; return QString(); }
                }
                i += 4;
                if (cp >= 0xd800 && cp <= 0xdbff && i + 6 <= n && s[i] == '\\' &&
                    s[i + 1] == 'u') {
                    unsigned int lo = 0;
                    bool ok = true;
                    for (int k = 0; k < 4; k++) {
                        const char h = s[i + 2 + k];
                        lo <<= 4;
                        if (h >= '0' && h <= '9') lo |= unsigned(h - '0');
                        else if (h >= 'a' && h <= 'f') lo |= unsigned(h - 'a' + 10);
                        else if (h >= 'A' && h <= 'F') lo |= unsigned(h - 'A' + 10);
                        else { ok = false; break; }
                    }
                    if (ok && lo >= 0xdc00 && lo <= 0xdfff) {
                        i += 6;
                        const unsigned int full =
                            0x10000 + ((cp - 0xd800) << 10) + (lo - 0xdc00);
                        out += QChar(ushort(0xd800 + ((full - 0x10000) >> 10)));
                        out += QChar(ushort(0xdc00 + ((full - 0x10000) & 0x3ff)));
                        break;
                    }
                }
                out += QChar(ushort(cp));
                break;
            }
            default:
                error = "unknown escape in JSON string";
                return QString();
            }
        }
        error = "unterminated string";
        return QString();
    }

    NodePtr parseBool(QString &error) {
        if (n - i >= 4 && strncmp(s + i, "true", 4) == 0) {
            i += 4;
            NodePtr node = makeNode(Node::Bool);
            node->boolean = true;
            return node;
        }
        if (n - i >= 5 && strncmp(s + i, "false", 5) == 0) {
            i += 5;
            return makeNode(Node::Bool);
        }
        error = "expected true/false";
        return NodePtr();
    }

    NodePtr parseNull(QString &error) {
        if (n - i >= 4 && strncmp(s + i, "null", 4) == 0) {
            i += 4;
            return makeNode(Node::Null);
        }
        error = "expected null";
        return NodePtr();
    }

    NodePtr parseNumber(QString &error) {
        const int start = i;
        if (i < n && (s[i] == '-' || s[i] == '+')) i++;
        while (i < n && ((s[i] >= '0' && s[i] <= '9') || s[i] == '.' || s[i] == 'e' ||
                         s[i] == 'E' || s[i] == '-' || s[i] == '+'))
            i++;
        if (i == start) {
            error = "unexpected character in JSON";
            return NodePtr();
        }
        const QByteArray text(s + start, i - start);
        NodePtr node = makeNode(Node::Number);
        node->number = text.toDouble();
        return node;
    }

    const char *s;
    int n;
    int i;
};

// ── Serialiser ───────────────────────────────────────────────────────────
void writeString(QByteArray &out, const QString &text) {
    out += '"';
    for (int i = 0; i < text.size(); i++) {
        const QChar ch = text.at(i);
        const ushort u = ch.unicode();
        switch (u) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\b': out += "\\b"; break;
        case '\f': out += "\\f"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (u < 0x20) {
                char buf[8];
                snprintf(buf, sizeof(buf), "\\u%04x", unsigned(u));
                out += buf;
            } else if (u < 0x80) {
                out += char(u);
            } else {
                // Encode as UTF-8 so the file stays readable outside Qt.
                if (u < 0x800) {
                    out += char(0xc0 | (u >> 6));
                    out += char(0x80 | (u & 0x3f));
                } else if (u >= 0xd800 && u <= 0xdbff && i + 1 < text.size()) {
                    const ushort lo = text.at(i + 1).unicode();
                    if (lo >= 0xdc00 && lo <= 0xdfff) {
                        const unsigned int cp =
                            0x10000 + ((unsigned(u) - 0xd800) << 10) + (lo - 0xdc00);
                        out += char(0xf0 | (cp >> 18));
                        out += char(0x80 | ((cp >> 12) & 0x3f));
                        out += char(0x80 | ((cp >> 6) & 0x3f));
                        out += char(0x80 | (cp & 0x3f));
                        i++;
                        break;
                    }
                    out += "\\ufffd";
                } else {
                    out += char(0xe0 | (u >> 12));
                    out += char(0x80 | ((u >> 6) & 0x3f));
                    out += char(0x80 | (u & 0x3f));
                }
            }
            break;
        }
    }
    out += '"';
}

void writeValue(QByteArray &out, const NodePtr &node, bool indented, int depth) {
    const QByteArray pad(indented ? depth * 2 : 0, ' ');
    const QByteArray inner(indented ? (depth + 1) * 2 : 0, ' ');
    const QByteArray newline(indented ? "\n" : "");
    if (!node) { out += "null"; return; }

    switch (node->kind) {
    case Node::Null: out += "null"; break;
    case Node::Bool: out += node->boolean ? "true" : "false"; break;
    case Node::Number: {
        char buf[32];
        const double value = node->number;
        if (value == double(qint64(value)) && std::abs(value) < 1e15)
            snprintf(buf, sizeof(buf), "%lld", static_cast<long long>(value));
        else
            snprintf(buf, sizeof(buf), "%.17g", value);
        out += buf;
        break;
    }
    case Node::String: writeString(out, node->text); break;
    case Node::Array: {
        if (node->items.empty()) { out += "[]"; break; }
        out += "[";
        out += newline;
        for (size_t k = 0; k < node->items.size(); k++) {
            if (k) { out += ","; out += newline; }
            out += inner;
            writeValue(out, node->items[k], indented, depth + 1);
        }
        out += newline;
        out += pad;
        out += "]";
        break;
    }
    case Node::Object: {
        if (node->fields.empty()) { out += "{}"; break; }
        out += "{";
        out += newline;
        for (size_t k = 0; k < node->fields.size(); k++) {
            if (k) { out += ","; out += newline; }
            out += inner;
            writeString(out, node->fields[k].first);
            out += ":";
            if (indented) out += " ";
            writeValue(out, node->fields[k].second, indented, depth + 1);
        }
        out += newline;
        out += pad;
        out += "}";
        break;
    }
    }
}

std::shared_ptr<Node> objectField(const NodePtr &node, const QString &key) {
    if (!node) return std::shared_ptr<Node>();
    for (size_t k = 0; k < node->fields.size(); k++) {
        if (node->fields[k].first == key) return node->fields[k].second;
    }
    return std::shared_ptr<Node>();
}

} // namespace
} // namespace json
} // namespace wa

// ── QJsonValue ───────────────────────────────────────────────────────────

QJsonValue::QJsonValue() {}

QJsonValue::QJsonValue(bool b) {
    d = wa::json::makeNode(wa::json::Node::Bool);
    d->boolean = b;
}

QJsonValue::QJsonValue(double value) {
    d = wa::json::makeNode(wa::json::Node::Number);
    d->number = value;
}

QJsonValue::QJsonValue(int value) {
    d = wa::json::makeNode(wa::json::Node::Number);
    d->number = value;
}

QJsonValue::QJsonValue(qint64 value) {
    d = wa::json::makeNode(wa::json::Node::Number);
    d->number = double(value);
}

QJsonValue::QJsonValue(const QString &text) {
    d = wa::json::makeNode(wa::json::Node::String);
    d->text = text;
}

QJsonValue::QJsonValue(const char *text) {
    d = wa::json::makeNode(wa::json::Node::String);
    d->text = QString::fromUtf8(text);
}

QJsonValue::QJsonValue(const QJsonObject &object) : d(object.node()) {}

QJsonValue::QJsonValue(const QJsonArray &array) : d(array.node()) {}

QJsonValue QJsonValue::fromNode(const wa::json::NodePtr &n) {
    QJsonValue value;
    value.d = n;
    return value;
}

QJsonValue::Type QJsonValue::type() const {
    if (!d) return Undefined;
    switch (d->kind) {
    case wa::json::Node::Null: return Null;
    case wa::json::Node::Bool: return Bool;
    case wa::json::Node::Number: return Double;
    case wa::json::Node::String: return String;
    case wa::json::Node::Array: return Array;
    case wa::json::Node::Object: return Object;
    }
    return Undefined;
}

bool QJsonValue::isNull() const { return type() == Null; }
bool QJsonValue::isBool() const { return type() == Bool; }
bool QJsonValue::isDouble() const { return type() == Double; }
bool QJsonValue::isString() const { return type() == String; }
bool QJsonValue::isArray() const { return type() == Array; }
bool QJsonValue::isObject() const { return type() == Object; }
bool QJsonValue::isUndefined() const { return !d; }

bool QJsonValue::toBool(bool defaultValue) const {
    return type() == Bool ? d->boolean : defaultValue;
}

double QJsonValue::toDouble(double defaultValue) const {
    return type() == Double ? d->number : defaultValue;
}

int QJsonValue::toInt(int defaultValue) const {
    return type() == Double ? int(d->number) : defaultValue;
}

qint64 QJsonValue::toInteger(qint64 defaultValue) const {
    return type() == Double ? qint64(d->number) : defaultValue;
}

QString QJsonValue::toString(const QString &defaultValue) const {
    return type() == String ? d->text : defaultValue;
}

QJsonArray QJsonValue::toArray() const {
    return type() == Array ? QJsonArray::fromNode(d) : QJsonArray();
}

QJsonObject QJsonValue::toObject() const {
    return type() == Object ? QJsonObject::fromNode(d) : QJsonObject();
}

QVariant QJsonValue::toVariant() const {
    switch (type()) {
    case Bool: return QVariant(d->boolean);
    case Double: return QVariant(d->number);
    case String: return QVariant(d->text);
    case Array: {
        QVariantList list;
        const std::vector<QJsonValue> items = toArray().values();
        for (size_t k = 0; k < items.size(); k++) list.append(items[k].toVariant());
        return QVariant(list);
    }
    case Object: {
        QVariantMap map;
        const std::vector<std::pair<QString, QJsonValue> > fields = toObject().entries();
        for (size_t k = 0; k < fields.size(); k++)
            map.insert(fields[k].first, fields[k].second.toVariant());
        return QVariant(map);
    }
    case Null:
    case Undefined: return QVariant();
    }
    return QVariant();
}

bool QJsonValue::operator==(const QJsonValue &other) const {
    if (type() != other.type()) return false;
    switch (type()) {
    case Undefined:
    case Null: return true;
    case Bool: return d->boolean == other.d->boolean;
    case Double: return d->number == other.d->number;
    case String: return d->text == other.d->text;
    default: return d == other.d;
    }
}

// ── QJsonObject ──────────────────────────────────────────────────────────

QJsonObject::QJsonObject() : d(wa::json::makeNode(wa::json::Node::Object)) {}

QJsonObject::QJsonObject(std::initializer_list<Entry> entries)
    : d(wa::json::makeNode(wa::json::Node::Object)) {
    for (std::initializer_list<Entry>::const_iterator it = entries.begin(); it != entries.end();
         ++it) {
        insert(it->first, it->second);
    }
}

QJsonObject QJsonObject::fromNode(const wa::json::NodePtr &n) {
    QJsonObject object;
    object.d = n ? n : object.d;
    return object;
}

int QJsonObject::size() const { return d ? int(d->fields.size()) : 0; }

bool QJsonObject::contains(const QString &key) const {
    if (!d) return false;
    for (size_t k = 0; k < d->fields.size(); k++) {
        if (d->fields[k].first == key) return true;
    }
    return false;
}

QJsonValue QJsonObject::value(const QString &key) const {
    return QJsonValue::fromNode(wa::json::objectField(d, key));
}

QJsonValue QJsonObject::operator[](const QString &key) const { return value(key); }

QJsonObject::value_ref QJsonObject::operator[](const QString &key) {
    return value_ref(this, key);
}

void QJsonObject::insert(const QString &key, const QJsonValue &value) {
    mut();
    for (size_t k = 0; k < d->fields.size(); k++) {
        if (d->fields[k].first == key) {
            d->fields[k].second = value.node();
            return;
        }
    }
    d->fields.push_back(std::make_pair(key, value.node()));
}

void QJsonObject::remove(const QString &key) {
    mut();
    for (size_t k = 0; k < d->fields.size(); k++) {
        if (d->fields[k].first == key) {
            d->fields.erase(d->fields.begin() + long(k));
            return;
        }
    }
}

QStringList QJsonObject::keys() const {
    QStringList out;
    if (!d) return out;
    for (size_t k = 0; k < d->fields.size(); k++) out << d->fields[k].first;
    return out;
}

std::vector<std::pair<QString, QJsonValue> > QJsonObject::entries() const {
    std::vector<std::pair<QString, QJsonValue> > out;
    if (!d) return out;
    for (size_t k = 0; k < d->fields.size(); k++) {
        out.push_back(std::make_pair(d->fields[k].first,
                                     QJsonValue::fromNode(d->fields[k].second)));
    }
    return out;
}

QJsonObject::value_ref &QJsonObject::value_ref::operator=(const QJsonValue &value) {
    m_object->insert(m_key, value);
    return *this;
}
QJsonObject::value_ref &QJsonObject::value_ref::operator=(const QJsonObject &value) {
    return operator=(QJsonValue(value));
}
QJsonObject::value_ref &QJsonObject::value_ref::operator=(const QJsonArray &value) {
    return operator=(QJsonValue(value));
}
QJsonObject::value_ref &QJsonObject::value_ref::operator=(const QString &value) {
    return operator=(QJsonValue(value));
}
QJsonObject::value_ref &QJsonObject::value_ref::operator=(const char *value) {
    return operator=(QJsonValue(value));
}
QJsonObject::value_ref &QJsonObject::value_ref::operator=(bool value) {
    return operator=(QJsonValue(value));
}
QJsonObject::value_ref &QJsonObject::value_ref::operator=(int value) {
    return operator=(QJsonValue(value));
}
QJsonObject::value_ref &QJsonObject::value_ref::operator=(qint64 value) {
    return operator=(QJsonValue(value));
}
QJsonObject::value_ref &QJsonObject::value_ref::operator=(double value) {
    return operator=(QJsonValue(value));
}

// ── QJsonArray ───────────────────────────────────────────────────────────

QJsonArray::QJsonArray() : d(wa::json::makeNode(wa::json::Node::Array)) {}

QJsonArray::QJsonArray(std::initializer_list<QJsonValue> values)
    : d(wa::json::makeNode(wa::json::Node::Array)) {
    for (std::initializer_list<QJsonValue>::const_iterator it = values.begin();
         it != values.end(); ++it)
        append(*it);
}

QJsonArray QJsonArray::fromNode(const wa::json::NodePtr &n) {
    QJsonArray array;
    array.d = n ? n : array.d;
    return array;
}

int QJsonArray::size() const { return d ? int(d->items.size()) : 0; }

void QJsonArray::append(const QJsonValue &value) {
    mut();
    d->items.push_back(value.node());
}

void QJsonArray::prepend(const QJsonValue &value) {
    mut();
    d->items.insert(d->items.begin(), value.node());
}

void QJsonArray::insert(int index, const QJsonValue &value) {
    mut();
    if (index < 0) index = 0;
    if (size_t(index) > d->items.size()) index = int(d->items.size());
    d->items.insert(d->items.begin() + index, value.node());
}

void QJsonArray::removeAt(int index) {
    mut();
    if (index >= 0 && size_t(index) < d->items.size())
        d->items.erase(d->items.begin() + index);
}

QJsonValue QJsonArray::at(int index) const {
    if (index < 0 || size_t(index) >= size()) return QJsonValue();
    return QJsonValue::fromNode(d->items[size_t(index)]);
}

QJsonValue QJsonArray::operator[](int index) const { return at(index); }

std::vector<QJsonValue> QJsonArray::values() const {
    std::vector<QJsonValue> out;
    if (!d) return out;
    for (size_t k = 0; k < d->items.size(); k++)
        out.push_back(QJsonValue::fromNode(d->items[k]));
    return out;
}

// ── QJsonDocument ────────────────────────────────────────────────────────

QJsonDocument::QJsonDocument() {}

QJsonDocument::QJsonDocument(const QJsonObject &object) : m_root(object) {}

QJsonDocument::QJsonDocument(const QJsonArray &array) : m_root(array) {}

QJsonDocument QJsonDocument::fromJson(const QByteArray &json) {
    QJsonDocument doc;
    wa::json::Parser parser(json);
    QString error;
    wa::json::NodePtr root = parser.parse(error);
    if (root) doc.m_root = QJsonValue::fromNode(root);
    return doc;
}

QByteArray QJsonDocument::toJson(JsonFormat format) const {
    QByteArray out;
    wa::json::writeValue(out, m_root.node(), format == Indented, 0);
    if (format == Indented) out += "\n";
    return out;
}

bool QJsonDocument::isNull() const { return m_root.isNull(); }
bool QJsonDocument::isEmpty() const {
    return m_root.isUndefined() ||
           (m_root.isObject() && m_root.toObject().isEmpty()) ||
           (m_root.isArray() && m_root.toArray().isEmpty());
}
bool QJsonDocument::isObject() const { return m_root.isObject(); }
bool QJsonDocument::isArray() const { return m_root.isArray(); }
QJsonObject QJsonDocument::object() const { return m_root.toObject(); }
QJsonArray QJsonDocument::array() const { return m_root.toArray(); }

#endif // QT_VERSION < 5.0.0
