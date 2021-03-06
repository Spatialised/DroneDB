/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#include "ddb.h"
#include "gdal_priv.h"
#include "../logger.h"

namespace ddb {

#define UPDATE_QUERY "UPDATE entries SET hash=?, type=?, meta=?, mtime=?, size=?, depth=?, point_geom=GeomFromText(?, 4326), polygon_geom=GeomFromText(?, 4326) WHERE path=?"

std::string create(const std::string &directory) {
    fs::path dirPath = directory;
    if (!fs::exists(dirPath)) throw FSException("Invalid directory: " + directory  + " (does not exist)");

    fs::path ddbDirPath = dirPath / ".ddb";
    if (directory == ".") ddbDirPath = ".ddb"; // Nicer to the eye
    fs::path dbasePath = ddbDirPath / "dbase.sqlite";

    try {
        LOGD << "Checking if .ddb directory exists...";
        if (fs::exists(ddbDirPath)) {
            throw FSException("Cannot initialize database: " + ddbDirPath.string() + " already exists");
        } else {
            if (fs::create_directory(ddbDirPath)) {
                LOGD << ddbDirPath.string() + " created";
            } else {
                throw FSException("Cannot create directory: " + ddbDirPath.string() + ". Check that you have the proper permissions?");
            }
        }

        LOGD << "Checking if dbase exists...";
        if (fs::exists(dbasePath)) {
            throw FSException(ddbDirPath.string() + " already exists");
        } else {
            LOGD << "Creating " << dbasePath.string();

            // Create database
            std::unique_ptr<Database> db = std::make_unique<Database>();
            db->open(dbasePath);
            db->createTables();
            db->close();

            return ddbDirPath;
        }
    } catch (const AppException &exception) {
        LOGV << "Exception caught, cleaning up...";

        throw exception;
    }
}

std::unique_ptr<Database> open(const std::string &directory, bool traverseUp = false) {
    fs::path dirPath = fs::absolute(directory);
    fs::path ddbDirPath = dirPath / ".ddb";
    fs::path dbasePath = ddbDirPath / "dbase.sqlite";

    if (fs::exists(dbasePath)) {
        LOGD << dbasePath.string() + " exists";

        std::unique_ptr<Database> db = std::make_unique<Database>();
        db->open(dbasePath);
        if (!db->tableExists("entries")) {
            throw DBException("Table 'entries' not found (not a valid database: " + dbasePath.string() + ")");
        }
        return db;
    } else if (traverseUp && dirPath.parent_path() != dirPath) {
        return open(dirPath.parent_path(), true);
    } else {
        throw FSException("Not a valid DroneDB directory, .ddb does not exist. Did you run ddb init?");
    }
}

fs::path rootDirectory(Database *db) {
    assert(db != nullptr);
    return fs::path(db->getOpenFile()).parent_path().parent_path();
}

// Computes a list of paths inside rootDirectory
// all paths must be subfolders/files within rootDirectory
// or an exception is thrown
// If includeDirs is true and the list includes paths to directories that are in paths
// eg. if path/to/file is in paths, both "path/" and "path/to"
// are includes in the result.
// ".ddb" files/dirs are always ignored and skipped.
// If a directory is in the input paths, they are included regardless of includeDirs
std::vector<fs::path> getIndexPathList(fs::path rootDirectory, const std::vector<std::string> &paths, bool includeDirs) {
    std::vector<fs::path> result;
    std::unordered_map<std::string, bool> directories;

    if (!utils::pathsAreChildren(rootDirectory, paths)) {
        throw FSException("Some paths are not contained within: " + rootDirectory.string() + ". Did you run ddb init?");
    }

    for (fs::path p : paths) {
        // fs::directory_options::skip_permission_denied
        if (p.filename() == ".ddb") continue;

        if (fs::is_directory(p)) {
            for(auto i = fs::recursive_directory_iterator(p);
                    i != fs::recursive_directory_iterator();
                    ++i ) {

                fs::path rp = i->path();

                // Skip .ddb
                if(rp.filename() == ".ddb") i.disable_recursion_pending();

                if (fs::is_directory(rp) && includeDirs) {
                    directories[rp.string()] = true;
                } else {
                    result.push_back(rp);
                }

                if (includeDirs) {
                    while(rp.has_parent_path()) {
                        rp = rp.parent_path();
                        directories[rp.string()] = true;
                    }
                }
            }

            directories[p.string()] = true;
        } else if (fs::exists(p)) {
            // File
            result.push_back(p);

            if (includeDirs) {
                while(p.has_parent_path()) {
                    p = p.parent_path();
                    directories[p.string()] = true;
                }
            }
        } else {
            throw FSException("Path does not exist: " + p.string());
        }
    }

    for (auto it : directories) {
        result.push_back(it.first);
    }

    return result;
}

std::vector<fs::path> getPathList(const std::vector<std::string> &paths, bool includeDirs, int maxDepth) {
    std::vector<fs::path> result;
    std::unordered_map<std::string, bool> directories;

    for (fs::path p : paths) {
        // fs::directory_options::skip_permission_denied
        if (p.filename() == ".ddb") continue;

        if (fs::is_directory(p)) {
            for(auto i = fs::recursive_directory_iterator(p);
                    i != fs::recursive_directory_iterator();
                    ++i ) {

                fs::path rp = i->path();

                // Skip .ddb
                if(rp.filename() == ".ddb") i.disable_recursion_pending();

                // Max depth
                if (maxDepth > 0 && i.depth() >= (maxDepth - 1)) i.disable_recursion_pending();

                if (fs::is_directory(rp)) {
                    if (includeDirs) result.push_back(rp);
                }else{
                    result.push_back(rp);
                }
            }
        } else if (fs::exists(p)) {
            // File
            result.push_back(p);
        } else {
            throw FSException("Path does not exist: " + p.string());
        }
    }

    return result;
}


bool checkUpdate(Entry &e, const fs::path &p, long long dbMtime, const std::string &dbHash) {
    bool folder = fs::is_directory(p);

    // Did it change?
    e.mtime = utils::getModifiedTime(p);

    if (e.mtime != dbMtime) {
        LOGD << p.string() << " modified time ( " << dbMtime << " ) differs from file value: " << e.mtime;

        if (folder) {
            // Don't check hashes for folders
            return true;
        } else {
            e.hash = Hash::fileSHA256(p);

            if (dbHash != e.hash) {
                LOGD << p.string() << " hash differs (old: " << dbHash << " | new: " << e.hash << ")";
                return true;
            }
        }
    }

    return false;
}

void doUpdate(Statement *updateQ, const Entry &e) {
    // Fields
    updateQ->bind(1, e.hash);
    updateQ->bind(2, e.type);
    updateQ->bind(3, e.meta.dump());
    updateQ->bind(4, static_cast<long long>(e.mtime));
    updateQ->bind(5, static_cast<long long>(e.size));
    updateQ->bind(6, e.depth);
    updateQ->bind(7, e.point_geom.toWkt());
    updateQ->bind(8, e.polygon_geom.toWkt());

    // Where
    updateQ->bind(9, e.path);

    updateQ->execute();
    std::cout << "U\t" << e.path << std::endl;
}

void addToIndex(Database *db, const std::vector<std::string> &paths) {
    fs::path directory = rootDirectory(db);
    auto pathList = getIndexPathList(directory, paths, true);

    auto q = db->query("SELECT mtime,hash FROM entries WHERE path=?");
    auto insertQ = db->query("INSERT INTO entries (path, hash, type, meta, mtime, size, depth, point_geom, polygon_geom) "
                             "VALUES (?, ?, ?, ?, ?, ?, ?, GeomFromText(?, 4326), GeomFromText(?, 4326))");
    auto updateQ = db->query(UPDATE_QUERY);
    db->exec("BEGIN TRANSACTION");

    ParseEntryOpts opts;
    opts.withHash = true;

    for (auto &p : pathList) {
        fs::path relPath = fs::weakly_canonical(fs::absolute(p).lexically_relative(fs::absolute(directory)));

        q->bind(1, relPath.generic_string());

        bool update = false;
        bool add = false;
        Entry e;

        if (q->fetch()) {
            // Entry exist, update if necessary
            update = checkUpdate(e, p, q->getInt64(0), q->getText(1));
        } else {
            // Brand new, add
            add = true;
        }

        if (add || update) {
            parseEntry(p, directory, e, opts);

            if (add) {
                insertQ->bind(1, e.path);
                insertQ->bind(2, e.hash);
                insertQ->bind(3, e.type);
                insertQ->bind(4, e.meta.dump());
                insertQ->bind(5, static_cast<long long>(e.mtime));
                insertQ->bind(6, static_cast<long long>(e.size));
                insertQ->bind(7, e.depth);
                insertQ->bind(8, e.point_geom.toWkt());
                insertQ->bind(9, e.polygon_geom.toWkt());

                insertQ->execute();
                std::cout << "A\t" << e.path << std::endl;
            } else {
                doUpdate(updateQ.get(), e);
            }
        }

        q->reset();
    }

    db->exec("COMMIT");
}

void removeFromIndex(Database *db, const std::vector<std::string> &paths) {
    fs::path directory = rootDirectory(db);
    auto pathList = getIndexPathList(directory, paths, false);

    auto q = db->query("DELETE FROM entries WHERE path = ?");
    db->exec("BEGIN TRANSACTION");

    for (auto &p : pathList) {
        fs::path relPath = fs::weakly_canonical(fs::absolute(p).lexically_relative(fs::absolute(directory)));
        q->bind(1, relPath.generic_string());
        q->execute();
        if (db->changes() >= 1) {
            std::cout << "D\t" << relPath.generic_string() << std::endl;
        }
    }

    db->exec("COMMIT");
}

void syncIndex(Database *db) {
    fs::path directory = rootDirectory(db);

    auto q = db->query("SELECT path,mtime,hash FROM entries");
    auto deleteQ = db->query("DELETE FROM entries WHERE path = ?");
    auto updateQ = db->query(UPDATE_QUERY);

    db->exec("BEGIN TRANSACTION");

    ParseEntryOpts opts;
    opts.withHash = true;

    while(q->fetch()) {
        fs::path relPath = q->getText(0);
        fs::path p = directory / relPath; // TODO: does this work on Windows?
        Entry e;

        if (fs::exists(p)) {
            if (checkUpdate(e, p, q->getInt64(1), q->getText(2))) {
                parseEntry(p, directory, e, opts);
                doUpdate(updateQ.get(), e);
            }
        } else {
            // Removed
            deleteQ->bind(1, relPath.generic_string());
            deleteQ->execute();
            std::cout << "D\t" << relPath.generic_string() << std::endl;
        }
    }

    db->exec("COMMIT");
}

std::string getVersion(){
    return "0.9.1";
}

// This must be called as the very first function
// of every DDB process/program
void initialize(){
    init_logger();
    Database::Initialize();
    exif::Initialize();
    GDALAllRegister();
}

}

