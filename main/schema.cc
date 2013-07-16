#include <functional>

#include <parser/lex_util.hh>
#include <parser/stringify.hh>
#include <main/schema.hh>
#include <main/rewrite_main.hh>
#include <main/init_onions.hh>

using namespace std;

ostream&
operator<<(ostream &out, const OnionLevelFieldPair &p)
{
    out << "(onion " << p.first
        << ", level " << levelnames[(int)p.second.first]
        << ", field `" << (p.second.second == NULL ? "*" : p.second.second->fname) << "`"
        << ")";
    return out;
}

std::ostream&
operator<<(std::ostream &out, const OLK &olk)
{
    out << "( onion " << olk.o << " level " << levelnames[(uint)olk.l] << " fieldmeta ";
    if (olk.key == NULL) {
	out << " NULL ";
    } else {
	out << olk.key->fname;
    }
    out << ")";
    return out;
}

std::string OnionMeta::getAnonOnionName() const
{
    return onionname;
}

FieldMeta::FieldMeta(std::string name, Create_field *field, AES_KEY *mKey)
    : fname(name), salt_name(BASE_SALT_NAME + getpRandomName())
{
    if (mKey) {
        init_onions(mKey, this, field);
    } else {
        init_onions(NULL, this, field);
    }
}

FieldMeta::~FieldMeta()
{
    auto cp = onions;
    onions.clear();

    for (auto it : cp) {
        delete it.second;
    }
}

string FieldMeta::stringify() const
{
    string res = " [FieldMeta " + fname + "]";
    return res;
}

TableMeta::~TableMeta()
{
    auto cp = fieldMetaMap;
    fieldMetaMap.clear();

    for (auto it : cp) {
        delete it.second;
    }
}

bool TableMeta::fieldMetaExists(std::string name)
{
    return this->fieldMetaMap.find(name) != this->fieldMetaMap.end();
}

bool TableMeta::addFieldMeta(FieldMeta *fm)
{
    if (fieldMetaExists(fm->fname)) {
        return false;
    }
    
    this->fieldMetaMap[fm->fname] = fm;
    this->fieldNames.push_back(fm->fname);//TODO: do we need fieldNames?

    return true;
}

FieldMeta *TableMeta::getFieldMeta(std::string field)
{
    auto it = fieldMetaMap.find(field);
    if (fieldMetaMap.end() == it) {
        return NULL;
    } else {
        return it->second;
    }
}

// FIXME: May run into problems where a plaintext table expects the regular
// name, but it shouldn't get that name from 'getAnonTableName' anyways.
std::string TableMeta::getAnonTableName() const {
    return anon_table_name;
}

bool TableMeta::destroyFieldMeta(std::string field)
{
    FieldMeta *fm = this->getFieldMeta(field);
    if (NULL == fm) {
        return false;
    }

    auto erase_count = fieldMetaMap.erase(field);
    fieldNames.remove(field);

    delete fm;
    return 1 == erase_count;
}

// TODO: Add salt.
std::string TableMeta::getAnonIndexName(std::string index_name) const
{
    std::string hash_input = anon_table_name + index_name;
    std::size_t hsh = std::hash<std::string>()(hash_input);

    return std::string("index_") + std::to_string(hsh);
}

SchemaInfo::~SchemaInfo()
{
    auto cp = tableMetaMap;
    tableMetaMap.clear();

    for (auto it : cp) {
        delete it.second;
    }
}

bool
SchemaInfo::addTableMeta(std::string name, TableMeta *tm)
{
    if (this->tableMetaExists(name)) {
        return false;
    }

    tableMetaMap[name] = tm;
    return true;
}

TableMeta *
SchemaInfo::getTableMeta(const string & table) const
{
    auto it = tableMetaMap.find(table);
    if (tableMetaMap.end() == it) {
        return NULL;
    } else {
        return it->second;
    }
}

FieldMeta *
SchemaInfo::getFieldMeta(const string & table, const string & field) const
{
    TableMeta * tm = getTableMeta(table);
    if (NULL == tm) {
        return NULL;
    }

    return tm->getFieldMeta(field);
}

bool
SchemaInfo::tableMetaExists(std::string table) const
{
    return tableMetaMap.find(table) != tableMetaMap.end();
}

bool
SchemaInfo::destroyTableMeta(std::string table)
{
    TableMeta *tm = this->getTableMeta(table);
    if (NULL == tm) {
        return false;
    }

    if (1 == tableMetaMap.erase(table)) {
        delete tm;
        return true;
    }

    return false;
}

