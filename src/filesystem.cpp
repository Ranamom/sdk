/**
 * @file filesystem.cpp
 * @brief Generic host filesystem access interfaces
 *
 * (c) 2013-2014 by Mega Limited, Auckland, New Zealand
 *
 * This file is part of the MEGA SDK - Client Access Engine.
 *
 * Applications using the MEGA API must present a valid application key
 * and comply with the the rules set forth in the Terms of Service.
 *
 * The MEGA SDK is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 *
 * @copyright Simplified (2-clause) BSD License.
 *
 * You should have received a copy of the license along with this
 * program.
 */
#include "mega/filesystem.h"
#include "mega/node.h"
#include "mega/megaclient.h"
#include "mega/logging.h"
#include "mega/mega_utf8proc.h"

namespace mega {
FileSystemAccess::FileSystemAccess()
    : waiter(NULL)
    , skip_errorreport(false)
    , transient_error(false)
    , notifyerr(false)
    , notifyfailed(false)
    , target_exists(false)
    , client(NULL)
{
}

void FileSystemAccess::captimestamp(m_time_t* t)
{
    // FIXME: remove upper bound before the year 2100 and upgrade server-side timestamps to BIGINT
    if (*t > (uint32_t)-1) *t = (uint32_t)-1;
    else if (*t < 0) *t = 0;
}

bool FileSystemAccess::islchex(char c) const
{
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
}

const char *FileSystemAccess::fstypetostring(const FileSystemType type) const
{
    switch (type)
    {
        case FS_NTFS:
            return "NTFS";
        case FS_EXFAT:
            return "EXFAT";
        case FS_FAT32:
            return "FAT32";
        case FS_EXT:
            return "EXT";
        case FS_HFS:
            return "HFS";
        case FS_APFS:
            return "APFS";
        case FS_DEFAULT:
            return "DEFAULT FS";
    }
}

FileSystemType FileSystemAccess::getlocalfstype(const string *dstPath) const
{
    if (!dstPath || dstPath->empty())
    {
        return FS_DEFAULT;
    }

#if defined (__linux__) || defined (__ANDROID__)
    struct statfs fileStat;
    if (!statfs(dstPath->c_str(), &fileStat))
    {
        switch (fileStat.f_type)
        {
            case EXT2_SUPER_MAGIC:
                return FS_EXT;
            case MSDOS_SUPER_MAGIC:
                return FS_FAT32;
            case HFS_SUPER_MAGIC:
                return FS_HFS;
            case NTFS_SB_MAGIC:
                return FS_NTFS;
            default:
                return FS_DEFAULT;
        }
    }
#elif defined  (__APPLE__) || defined (USE_IOS)
    struct statfs fileStat;
    if (!statfs(dstPath->c_str(), &fileStat))
    {
        if (!strcmp(fileStat.f_fstypename, "apfs"))
        {
            return FS_APFS;
        }
        if (!strcmp(fileStat.f_fstypename, "hfs"))
        {
            return FS_HFS;
        }
        if (!strcmp(fileStat.f_fstypename, "ntfs"))
        {
            return FS_NTFS;
        }
        if (!strcmp(fileStat.f_fstypename, "msdos"))
        {
            return FS_FAT32;
        }
    }
#elif defined(_WIN32) || defined(_WIN64) || defined(WINDOWS_PHONE)
    TCHAR volumeName[MAX_PATH + 1] = { 0 };
    TCHAR fileSystemName[MAX_PATH + 1] = { 0 };
    DWORD serialNumber = 0;
    DWORD maxComponentLen = 0;
    DWORD fileSystemFlags = 0;

    if (GetVolumeInformationA(dstPath->c_str(), volumeName, sizeof(volumeName),
                             &serialNumber, &maxComponentLen, &fileSystemFlags,
                             fileSystemName, sizeof(fileSystemName)) == true)
    {
        if (!strcmp(fileSystemName, "NTFS"))
        {
            return FS_NTFS;
        }
        if (!strcmp(fileSystemName, "exFAT"))
        {
            return FS_EXFAT;
        }
        if (!strcmp(fileSystemName, "FAT32"))
        {
            return FS_FAT32;
        }
    }
#endif
    return FS_DEFAULT;
}

// Group different filesystems types in families, according to its restricted charsets
bool FileSystemAccess::islocalfscompatible(unsigned char c, const FileSystemType fileSystemType) const
{
    switch (fileSystemType)
    {
        case FS_APFS:
        case FS_HFS:
            // APFS, HFS, HFS+ restricted characters => :
            return c != '\x3A';
        case FS_EXT:
            // ext2/ext3/ext4 restricted characters =>  / NULL
            return c != '\x00' && c != '\x2F';
        case FS_FAT32:
            // FAT32 restricted characters => " * / : < > ? \ | + , . ; = [ ]
            return !strchr("\\/:?\"<>|*+,.;=[]", c);
        case FS_EXFAT:
        case FS_NTFS:
        case FS_DEFAULT:
            // ExFAT, NTFS restricted characters => " * / : < > ? \ |
            // If filesystem couldn't be detected we'll use a restrictive charset to avoid issues.
            return !strchr("\\/:?\"<>|*", c);
    }
}

string FileSystemAccess::getValidPath(const string *path) const
{
    string validPath = path ? (*path) : "";
    string separator = getPathSeparator();
    if (separator != "/" && separator != "\\")
    {
        return validPath;
    }

    if (!validPath.empty())
    {
        size_t pos = validPath.rfind(separator);
        if (pos != std::string::npos && pos != validPath.size() - 1)
        {
            validPath = validPath.substr(0, pos + 1);
        }
    }
    return validPath;
}

// replace characters that are not allowed in local fs names with a %xx escape sequence
void FileSystemAccess::escapefsincompatible(string* name, const string *dstPath) const
{
    string validPath = getValidPath(dstPath);
    if (!name->compare(".."))
    {
        name->replace(0, 2, "%2e%2e");
        return;
    }
    if (!name->compare("."))
    {
        name->replace(0, 1, "%2e");
        return;
    }

    char buf[4];
    FileSystemType fileSystemType = getlocalfstype(&validPath);
    size_t utf8seqsize = 0;
    size_t i = 0;
    unsigned char c = '0';
    while (i < name->size())
    {
        c = static_cast<unsigned char>((*name)[i]);
        utf8seqsize = Utils::utf8SequenceSize(c);
        assert (utf8seqsize);
        if (utf8seqsize == 1 && !islocalfscompatible(c, fileSystemType))
        {
            std::string inc = name->substr(i, 1);
            sprintf(buf, "%%%02x", c);
            name->replace(i, 1, buf);
            LOG_debug << "Escape incompatible character for filesystem type "
                      << fstypetostring(fileSystemType)
                      << ", replace '" << inc << "' by '" << buf << "'\n";
        }
        i += utf8seqsize;
    }
}

void FileSystemAccess::unescapefsincompatible(string *name, const string *localPath) const
{
    string validPath = getValidPath(localPath);
    if (!name->compare("%2e%2e"))
    {
        name->replace(0, 6, "..");
        return;
    }
    if (!name->compare("%2e"))
    {
        name->replace(0, 3, ".");
        return;
    }

    FileSystemType fileSystemType = getlocalfstype(&validPath);
    for (int i = int(name->size()) - 2; i-- > 0; )
    {
        // conditions for unescaping: %xx must be well-formed
        if ((*name)[i] == '%' && islchex((*name)[i + 1]) && islchex((*name)[i + 2]))
        {
            char c = static_cast<char>((MegaClient::hexval((*name)[i + 1]) << 4) + MegaClient::hexval((*name)[i + 2]));

            if (!islocalfscompatible(static_cast<unsigned char>(c), fileSystemType))
            {
                std::string inc = name->substr(i, 3);
                name->replace(i, 3, &c, 1);
                LOG_debug << "Unescape incompatible character for filesystem type "
                          << fstypetostring(fileSystemType)
                          << ", replace '" << inc << "' by '" << name->substr(i, 1) << "'\n";
            }
        }
    }
}

std::string FileSystemAccess::getPathSeparator()
{
#if defined (__linux__) || defined (__ANDROID__) || defined  (__APPLE__) || defined (USE_IOS)
return "/";
#elif defined(_WIN32) || defined(_WIN64) || defined(WINDOWS_PHONE)
return "\\";
#elif
// Default case
LOG_warn << "No path separator found";
return "\\/";
#endif
}

// escape forbidden characters, then convert to local encoding
void FileSystemAccess::name2local(string* filename, const string *dstPath) const
{
    assert(filename);

    escapefsincompatible(filename, dstPath);

    string t = *filename;

    path2local(&t, filename);
}

void FileSystemAccess::normalize(string* filename) const
{
    if (!filename) return;

    const char* cfilename = filename->c_str();
    size_t fnsize = filename->size();
    string result;

    for (size_t i = 0; i < fnsize; )
    {
        // allow NUL bytes between valid UTF-8 sequences
        if (!cfilename[i])
        {
            result.append("", 1);
            i++;
            continue;
        }

        const char* substring = cfilename + i;
        char* normalized = (char*)utf8proc_NFC((uint8_t*)substring);

        if (!normalized)
        {
            filename->clear();
            return;
        }

        result.append(normalized);
        free(normalized);

        i += strlen(substring);
    }

    *filename = std::move(result);
}

// convert from local encoding, then unescape escaped forbidden characters
void FileSystemAccess::local2name(string *filename, const string *localPath) const
{
    assert(filename);

    string t = *filename;

    local2path(&t, filename);

    unescapefsincompatible(filename, localPath);
}

std::unique_ptr<string> FileSystemAccess::fsShortname(string& localname)
{
    string s;
    if (getsname(&localname, &s))
    {
        return ::mega::make_unique<string>(std::move(s));
    }
    return nullptr;
}

// default DirNotify: no notification available
DirNotify::DirNotify(string* clocalbasepath, string* cignore)
{
    localbasepath = *clocalbasepath;
    ignore = *cignore;

    failed = 1;
    failreason = "Not initialized";
    error = 0;
    sync = NULL;
}

// notify base LocalNode + relative path/filename
void DirNotify::notify(notifyqueue q, LocalNode* l, const char* localpath, size_t len, bool immediate)
{
    string path;
    path.assign(localpath, len);

#ifdef ENABLE_SYNC
    if ((q == DirNotify::DIREVENTS || q == DirNotify::EXTRA)
            && notifyq[q].size()
            && notifyq[q].back().localnode == l
            && notifyq[q].back().path == path)
    {
        if (notifyq[q].back().timestamp)
        {
            notifyq[q].back().timestamp = immediate ? 0 : Waiter::ds;
        }
        LOG_debug << "Repeated notification skipped";
        return;
    }

    if (!immediate && sync && !sync->initializing && q == DirNotify::DIREVENTS)
    {
        string tmppath;
        if (l)
        {
            l->getlocalpath(&tmppath);
        }

        if (localpath)
        {
            if (tmppath.size())
            {
                tmppath.append(sync->client->fsaccess->localseparator);
            }

            tmppath.append(path);
        }
        attr_map::iterator ait;
        auto fa = sync->client->fsaccess->newfileaccess(false);
        bool success = fa->fopen(&tmppath, false, false);
        LocalNode *ll = sync->localnodebypath(l, &path);
        if ((!ll && !success && !fa->retry) // deleted file
            || (ll && success && ll->node && ll->node->localnode == ll
                && (ll->type != FILENODE || (*(FileFingerprint *)ll) == (*(FileFingerprint *)ll->node))
                && (ait = ll->node->attrs.map.find('n')) != ll->node->attrs.map.end()
                && ait->second == ll->name
                && fa->fsidvalid && fa->fsid == ll->fsid && fa->type == ll->type
                && (ll->type != FILENODE || (ll->mtime == fa->mtime && ll->size == fa->size))))
        {
            LOG_debug << "Self filesystem notification skipped";
            return;
        }
    }

    if (q == DirNotify::DIREVENTS || q == DirNotify::EXTRA)
    {
        sync->client->syncactivity = true;
    }
#endif

    notifyq[q].resize(notifyq[q].size() + 1);
    notifyq[q].back().timestamp = immediate ? 0 : Waiter::ds;
    notifyq[q].back().localnode = l;
    notifyq[q].back().path = path;
}

// default: no fingerprint
fsfp_t DirNotify::fsfingerprint() const
{
    return 0;
}

bool DirNotify::fsstableids() const
{
    return true;
}

DirNotify* FileSystemAccess::newdirnotify(string* localpath, string* ignore)
{
    return new DirNotify(localpath, ignore);
}

FileAccess::FileAccess(Waiter *waiter)
{
    this->waiter = waiter;
    this->isAsyncOpened = false;
    this->numAsyncReads = 0;
}

FileAccess::~FileAccess()
{
    // All AsyncIOContext objects must be deleted before
    assert(!numAsyncReads && !isAsyncOpened);
}

// open file for reading
bool FileAccess::fopen(string* name)
{
    nonblocking_localname.resize(1);
    updatelocalname(name);

    return sysstat(&mtime, &size);
}

bool FileAccess::isfolder(string *name)
{
    fopen(name);
    return (type == FOLDERNODE);
}

// check if size and mtime are unchanged, then open for reading
bool FileAccess::openf()
{
    if (!nonblocking_localname.size())
    {
        // file was not opened in nonblocking mode
        return true;
    }

    m_time_t curr_mtime;
    m_off_t curr_size;
    if (!sysstat(&curr_mtime, &curr_size))
    {
        LOG_warn << "Error opening sync file handle (sysstat) "
                 << curr_mtime << " - " << mtime
                 << curr_size  << " - " << size;
        return false;
    }

    if (curr_mtime != mtime || curr_size != size)
    {
        mtime = curr_mtime;
        size = curr_size;
        retry = false;
        return false;
    }

    return sysopen();
}

void FileAccess::closef()
{
    if (nonblocking_localname.size())
    {
        sysclose();
    }
}

void FileAccess::asyncopfinished(void *param)
{
    Waiter *waiter = (Waiter *)param;
    if (waiter)
    {
        waiter->notify();
    }
}

AsyncIOContext *FileAccess::asyncfopen(string *f)
{
    nonblocking_localname.resize(1);
    updatelocalname(f);

    LOG_verbose << "Async open start";
    AsyncIOContext *context = newasynccontext();
    context->op = AsyncIOContext::OPEN;
    context->access = AsyncIOContext::ACCESS_READ;

    context->buffer = (byte *)f->data();
    context->len = static_cast<unsigned>(f->size());
    context->waiter = waiter;
    context->userCallback = asyncopfinished;
    context->userData = waiter;
    context->pos = size;
    context->fa = this;

    context->failed = !sysstat(&mtime, &size);
    context->retry = this->retry;
    context->finished = true;
    context->userCallback(context->userData);
    return context;
}

bool FileAccess::asyncopenf()
{
    numAsyncReads++;
    if (!nonblocking_localname.size())
    {
        return true;
    }

    if (isAsyncOpened)
    {
        return true;
    }

    m_time_t curr_mtime = 0;
    m_off_t curr_size = 0;
    if (!sysstat(&curr_mtime, &curr_size))
    {
        LOG_warn << "Error opening async file handle (sysstat) "
                 << curr_mtime << " - " << mtime
                 << curr_size  << " - " << size;
        return false;
    }

    if (curr_mtime != mtime || curr_size != size)
    {
        mtime = curr_mtime;
        size = curr_size;
        retry = false;
        return false;
    }

    LOG_debug << "Opening async file handle for reading";
    bool result = sysopen(true);
    if (result)
    {
        isAsyncOpened = true;
    }
    else
    {
        LOG_warn << "Error opening async file handle (sysopen)";
    }
    return result;
}

void FileAccess::asyncclosef()
{
    numAsyncReads--;
    if (isAsyncOpened && !numAsyncReads)
    {
        LOG_debug << "Closing async file handle";
        isAsyncOpened = false;
        sysclose();
    }
}

AsyncIOContext *FileAccess::asyncfopen(string *f, bool read, bool write, m_off_t pos)
{
    LOG_verbose << "Async open start";
    AsyncIOContext *context = newasynccontext();
    context->op = AsyncIOContext::OPEN;
    context->access = AsyncIOContext::ACCESS_NONE
            | (read ? AsyncIOContext::ACCESS_READ : 0)
            | (write ? AsyncIOContext::ACCESS_WRITE : 0);

    context->buffer = (byte *)f->data();
    context->len = static_cast<unsigned>(f->size());
    context->waiter = waiter;
    context->userCallback = asyncopfinished;
    context->userData = waiter;
    context->pos = pos;
    context->fa = this;

    asyncsysopen(context);
    return context;
}

void FileAccess::asyncsysopen(AsyncIOContext *context)
{
    context->failed = true;
    context->retry = false;
    context->finished = true;
    if (context->userCallback)
    {
        context->userCallback(context->userData);
    }
}

AsyncIOContext *FileAccess::asyncfread(string *dst, unsigned len, unsigned pad, m_off_t pos)
{
    LOG_verbose << "Async read start";
    dst->resize(len + pad);

    AsyncIOContext *context = newasynccontext();
    context->op = AsyncIOContext::READ;
    context->pos = pos;
    context->len = len;
    context->pad = pad;
    context->buffer = (byte *)dst->data();
    context->waiter = waiter;
    context->userCallback = asyncopfinished;
    context->userData = waiter;
    context->fa = this;

    if (!asyncopenf())
    {
        LOG_err << "Error in asyncopenf";
        context->failed = true;
        context->retry = this->retry;
        context->finished = true;
        context->userCallback(context->userData);
        return context;
    }

    asyncsysread(context);
    return context;
}

void FileAccess::asyncsysread(AsyncIOContext *context)
{
    context->failed = true;
    context->retry = false;
    context->finished = true;
    if (context->userCallback)
    {
        context->userCallback(context->userData);
    }
}

AsyncIOContext *FileAccess::asyncfwrite(const byte* data, unsigned len, m_off_t pos)
{
    LOG_verbose << "Async write start";

    AsyncIOContext *context = newasynccontext();
    context->op = AsyncIOContext::WRITE;
    context->pos = pos;
    context->len = len;
    context->buffer = (byte *)data;
    context->waiter = waiter;
    context->userCallback = asyncopfinished;
    context->userData = waiter;
    context->fa = this;

    asyncsyswrite(context);
    return context;
}

void FileAccess::asyncsyswrite(AsyncIOContext *context)
{
    context->failed = true;
    context->retry = false;
    context->finished = true;
    if (context->userCallback)
    {
        context->userCallback(context->userData);
    }
}

AsyncIOContext *FileAccess::newasynccontext()
{
    return new AsyncIOContext();
}

bool FileAccess::fread(string* dst, unsigned len, unsigned pad, m_off_t pos)
{
    if (!openf())
    {
        return false;
    }

    bool r;

    dst->resize(len + pad);

    if ((r = sysread((byte*)dst->data(), len, pos)))
    {
        memset((char*)dst->data() + len, 0, pad);
    }

    closef();

    return r;
}

bool FileAccess::frawread(byte* dst, unsigned len, m_off_t pos, bool caller_opened)
{
    if (!caller_opened && !openf())
    {
        return false;
    }

    bool r = sysread(dst, len, pos);

    if (!caller_opened)
    {
        closef();
    }

    return r;
}

AsyncIOContext::AsyncIOContext()
{
    op = NONE;
    pos = 0;
    len = 0;
    pad = 0;
    buffer = NULL;
    waiter = NULL;
    access = ACCESS_NONE;

    userCallback = NULL;
    userData = NULL;
    finished = false;
    failed = false;
    retry = false;
}

AsyncIOContext::~AsyncIOContext()
{
    finish();

    // AsyncIOContext objects must be deleted before the FileAccess object
    if (op == AsyncIOContext::READ)
    {
        fa->asyncclosef();
    }
}

void AsyncIOContext::finish()
{
    if (!finished)
    {
        while (!finished)
        {
            waiter->init(NEVER);
            waiter->wait();
        }

        // We could have been consumed and external event
        waiter->notify();
    }
}

FileInputStream::FileInputStream(FileAccess *fileAccess)
{
    this->fileAccess = fileAccess;
    this->offset = 0;
}

m_off_t FileInputStream::size()
{
    return fileAccess->size;
}

bool FileInputStream::read(byte *buffer, unsigned size)
{
    if (!buffer)
    {
        if ((offset + size) <= fileAccess->size)
        {
            offset += size;
            return true;
        }

        LOG_warn << "Invalid seek on FileInputStream";
        return false;
    }

    if (fileAccess->frawread(buffer, size, offset, true))
    {
        offset += size;
        return true;
    }

    LOG_warn << "Invalid read on FileInputStream";
    return false;
}

} // namespace
