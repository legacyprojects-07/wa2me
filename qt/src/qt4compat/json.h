// QJson* for Qt 4.7.
//
// QJsonDocument/QJsonObject/QJsonArray/QJsonValue only exist from Qt 5.0 and
// the Symbian toolchain ships Qt 4.7.4, so this header provides the same
// names and the subset of the Qt 5 API this client uses. It is a small
// value-tree (shared nodes, copy-on-write) plus a JSON parser/serialiser, so
// nothing depends on an external JSON library.
//
// Only compiled for Qt 4; with Qt 5+ the real classes are used.
#pragma once

#include <QtGlobal>

#if QT_VERSION < QT_VERSION_CHECK(5, 0, 0)

#include <QByteArray>
#include <QString>
#include <QStringList>
#include <QVariant>

#include <initializer_list>
#include <memory>
#include <utility>
#include <vector>

namespace wa {
namespace json {

// One JSON node. Objects keep insertion order, like Qt 5 does.
struct Node {
    enum Kind { Null, Bool, Number, String, Array, Object } kind;
    bool boolean;
    double number;
    QString text;
    std::vector<std::shared_ptr<Node> > items;                       // array
    std::vector<std::pair<QString, std::shared_ptr<Node> > > fields;  // object

    Node() : kind(Null), boolean(false), number(0) {}
};

typedef std::shared_ptr<Node> NodePtr;

inline NodePtr makeNode(Node::Kind kind) {
    NodePtr n(new Node);
    n->kind = kind;
    return n;
}

// Copy-on-write helper: returns a node that this container may modify.
NodePtr detach(const NodePtr &node);

} // namespace json
} // namespace wa

class QJsonValue;
class QJsonObject;
class QJsonArray;

class QJsonValue {
public:
    enum Type { Null, Bool, Double, String, Array, Object, Undefined };

    QJsonValue();
    QJsonValue(bool b);
    QJsonValue(double d);
    QJsonValue(int i);
    QJsonValue(qint64 i);
    QJsonValue(const QString &s);
    QJsonValue(const char *s);
    QJsonValue(const QJsonObject &a);
    QJsonValue(const QJsonArray &a);

    Type type() const;
    bool isNull() const;
    bool isBool() const;
    bool isDouble() const;
    bool isString() const;
    bool isArray() const;
    bool isObject() const;
    bool isUndefined() const;

    bool toBool(bool defaultValue = false) const;
    double toDouble(double defaultValue = 0) const;
    int toInt(int defaultValue = 0) const;
    qint64 toInteger(qint64 defaultValue = 0) const;
    QString toString(const QString &defaultValue = QString()) const;
    QJsonArray toArray() const;
    QJsonObject toObject() const;

    QVariant toVariant() const;

    bool operator==(const QJsonValue &other) const;

    wa::json::NodePtr node() const { return d; }
    static QJsonValue fromNode(const wa::json::NodePtr &n);

private:
    wa::json::NodePtr d;   // null == Undefined
};

class QJsonObject {
public:
    typedef std::pair<QString, QJsonValue> Entry;

    QJsonObject();
    QJsonObject(std::initializer_list<Entry> entries);

    int size() const;
    int count() const { return size(); }
    bool isEmpty() const { return size() == 0; }
    bool contains(const QString &key) const;
    QJsonValue value(const QString &key) const;
    QJsonValue operator[](const QString &key) const;
    void insert(const QString &key, const QJsonValue &value);
    void remove(const QString &key);
    QStringList keys() const;

    // Iteration yields the object's values (Qt 5 semantics).
    std::vector<std::pair<QString, QJsonValue> > entries() const;

    class const_iterator {
    public:
        const_iterator(const QJsonObject *object, int index)
            : m_entries(object->entries()), m_index(index) {}
        QJsonValue operator*() const { return m_entries[size_t(m_index)].second; }
        const_iterator &operator++() { ++m_index; return *this; }
        bool operator!=(const const_iterator &o) const { return m_index != o.m_index; }

    private:
        std::vector<std::pair<QString, QJsonValue> > m_entries;
        int m_index;
    };
    const_iterator begin() const { return const_iterator(this, 0); }
    const_iterator end() const { return const_iterator(this, size()); }

    // `o["key"] = value;`
    class value_ref {
    public:
        value_ref(QJsonObject *object, const QString &key) : m_object(object), m_key(key) {}
        QJsonValue toValue() const { return m_object->value(m_key); }
        operator QJsonValue() const { return toValue(); }
        value_ref &operator=(const QJsonValue &value);
        value_ref &operator=(const QJsonObject &value);
        value_ref &operator=(const QJsonArray &value);
        value_ref &operator=(const QString &value);
        value_ref &operator=(const char *value);
        value_ref &operator=(bool value);
        value_ref &operator=(int value);
        value_ref &operator=(qint64 value);
        value_ref &operator=(double value);

    private:
        QJsonObject *m_object;
        QString m_key;
    };
    value_ref operator[](const QString &key);

    wa::json::NodePtr node() const { return d; }
    static QJsonObject fromNode(const wa::json::NodePtr &n);

private:
    mutable wa::json::NodePtr d;
    void mut() { d = wa::json::detach(d); }
};

class QJsonArray {
public:
    QJsonArray();
    QJsonArray(std::initializer_list<QJsonValue> values);

    int size() const;
    int count() const { return size(); }
    bool isEmpty() const { return size() == 0; }
    void append(const QJsonValue &value);
    void prepend(const QJsonValue &value);
    void insert(int index, const QJsonValue &value);
    void removeAt(int index);
    QJsonValue at(int index) const;
    QJsonValue operator[](int index) const;

    std::vector<QJsonValue> values() const;

    // Range-for support (Qt 5's QJsonArray is iterable).
    class const_iterator {
    public:
        const_iterator(const QJsonArray *array, int index) : m_array(array), m_index(index) {}
        QJsonValue operator*() const { return m_array->at(m_index); }
        const_iterator &operator++() { ++m_index; return *this; }
        bool operator!=(const const_iterator &o) const { return m_index != o.m_index; }

    private:
        const QJsonArray *m_array;
        int m_index;
    };
    const_iterator begin() const { return const_iterator(this, 0); }
    const_iterator end() const { return const_iterator(this, size()); }

    wa::json::NodePtr node() const { return d; }
    static QJsonArray fromNode(const wa::json::NodePtr &n);

private:
    mutable wa::json::NodePtr d;
    void mut() { d = wa::json::detach(d); }
};

class QJsonDocument {
public:
    enum JsonFormat { Indented, Compact };

    QJsonDocument();
    explicit QJsonDocument(const QJsonObject &object);
    explicit QJsonDocument(const QJsonArray &array);

    static QJsonDocument fromJson(const QByteArray &json);
    QByteArray toJson(JsonFormat format = Indented) const;

    bool isNull() const;
    bool isEmpty() const;
    bool isObject() const;
    bool isArray() const;
    QJsonObject object() const;
    QJsonArray array() const;

private:
    QJsonValue m_root;
};

#endif // QT_VERSION < 5.0.0
