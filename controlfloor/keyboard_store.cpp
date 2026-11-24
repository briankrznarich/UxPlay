// SPDX-License-Identifier: GPL-3.0-or-later
#include "keyboard_store.h"
#include <sqlite3.h>
#include <sys/stat.h>
#include <stdexcept>
namespace controlfloor {
namespace {
struct Statement {
    sqlite3_stmt *stmt = nullptr;
    Statement(sqlite3 *db, const char *sql) {
        if (sqlite3_prepare_v2(db,sql,-1,&stmt,nullptr)!=SQLITE_OK) throw std::runtime_error(sqlite3_errmsg(db));
    }
    ~Statement() { sqlite3_finalize(stmt); }
    void text(int i,const std::string &s) { sqlite3_bind_text(stmt,i,s.c_str(),-1,SQLITE_TRANSIENT); }
};
}
KeyboardStore::~KeyboardStore() { if (db) sqlite3_close(db); }
void KeyboardStore::open(const std::string &path) {
    if (sqlite3_open(path.c_str(),&db)!=SQLITE_OK) throw std::runtime_error("Cannot open keyboard registry: "+std::string(sqlite3_errmsg(db)));
    chmod(path.c_str(),0600);
    sqlite3_busy_timeout(db,2000);
    const char *sql="PRAGMA journal_mode=WAL; CREATE TABLE IF NOT EXISTS keyboards ("
        "source TEXT NOT NULL, host TEXT NOT NULL, record TEXT NOT NULL, PRIMARY KEY(source,host)); PRAGMA user_version=1;";
    if (sqlite3_exec(db,sql,nullptr,nullptr,nullptr)!=SQLITE_OK) throw std::runtime_error(sqlite3_errmsg(db));
}
std::vector<nlohmann::json> KeyboardStore::load(const std::string &source) {
    Statement query(db,"SELECT record FROM keyboards WHERE source=? ORDER BY host LIMIT 256"); query.text(1,source);
    std::vector<nlohmann::json> records;
    int result;
    while ((result=sqlite3_step(query.stmt))==SQLITE_ROW) {
        const char *text=reinterpret_cast<const char *>(sqlite3_column_text(query.stmt,0));
        records.push_back(nlohmann::json::parse(text ? text : "{}"));
    }
    if (result!=SQLITE_DONE) throw std::runtime_error(sqlite3_errmsg(db));
    return records;
}
void KeyboardStore::save(const std::string &source,const nlohmann::json &record) {
    Statement query(db,"INSERT INTO keyboards(source,host,record) VALUES(?,?,?) "
        "ON CONFLICT(source,host) DO UPDATE SET record=excluded.record");
    query.text(1,source); query.text(2,record.at("host").get<std::string>()); query.text(3,record.dump());
    if (sqlite3_step(query.stmt)!=SQLITE_DONE) throw std::runtime_error(sqlite3_errmsg(db));
}
}
