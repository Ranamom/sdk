/**
 * @file node.cpp
 * @brief Classes for accessing local and remote nodes
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

#include "mega/node.h"
#include "mega/megaclient.h"
#include "mega/megaapp.h"
#include "mega/share.h"
#include "mega/serialize64.h"
#include "mega/base64.h"
#include "mega/sync.h"
#include "mega/transfer.h"
#include "mega/transferslot.h"
#include "mega/logging.h"
#include "mega/heartbeats.h"
#include "megafs.h"

namespace mega {

Node::Node(MegaClient& cclient, NodeHandle h, NodeHandle ph,
           nodetype_t t, m_off_t s, handle u, const char* fa, m_time_t ts)
    : client(&cclient)
{
    outshares = NULL;
    pendingshares = NULL;
    tag = 0;
    appdata = NULL;

    nodehandle = h.as8byte();
    parenthandle = ph.as8byte();

    parent = NULL;

#ifdef ENABLE_SYNC
    syncget = NULL;

    syncdeleted = SYNCDEL_NONE;
    todebris_it = client->todebris.end();
    tounlink_it = client->tounlink.end();
#endif

    type = t;

    size = s;
    owner = u;

    JSON::copystring(&fileattrstring, fa);

    ctime = ts;

    inshare = NULL;
    sharekey = NULL;
    foreignkey = false;

    plink = NULL;

    memset(&changed, 0, sizeof changed);

    mFingerPrintPosition = client->mNodeManager.getInvalidPosition();

    if (type == FILENODE)
    {
        mCounter.files = 1;
        mCounter.storage = size;
    }
    else if (type == FOLDERNODE)
    {
        mCounter.folders = 1;
    }
}

Node::~Node()
{
    if (keyApplied())
    {
        client->mAppliedKeyNodeCount--;
        assert(client->mAppliedKeyNodeCount >= 0);
    }

    // abort pending direct reads
    client->preadabort(this);

#ifdef ENABLE_SYNC
    // remove from todebris node_set
    if (todebris_it != client->todebris.end())
    {
        client->todebris.erase(todebris_it);
    }

    // remove from tounlink node_set
    if (tounlink_it != client->tounlink.end())
    {
        client->tounlink.erase(tounlink_it);
    }
#endif

    if (outshares)
    {
        // delete outshares, including pointers from users for this node
        for (share_map::iterator it = outshares->begin(); it != outshares->end(); it++)
        {
            delete it->second;
        }
        delete outshares;
    }

    if (pendingshares)
    {
        // delete pending shares
        for (share_map::iterator it = pendingshares->begin(); it != pendingshares->end(); it++)
        {
            delete it->second;
        }
        delete pendingshares;
    }

    delete plink;
    delete inshare;
    delete sharekey;

#ifdef ENABLE_SYNC
    // sync: remove reference from local filesystem node
    if (localnode)
    {
        localnode->deleted = true;
        localnode.reset();
    }

    // in case this node is currently being transferred for syncing: abort transfer
    delete syncget;
#endif
}

int Node::getShareType() const
{
    int shareType = ShareType_t::NO_SHARES;

    if (inshare)
    {
        shareType |= ShareType_t::IN_SHARES;
    }
    else
    {
        if (outshares)
        {
            for (share_map::iterator it = outshares->begin(); it != outshares->end(); it++)
            {
                Share *share = it->second;
                if (share->user)    // folder links are shares without user
                {
                    shareType |= ShareType_t::OUT_SHARES;
                    break;
                }
            }
        }
        if (pendingshares && pendingshares->size())
        {
            shareType |= ShareType_t::PENDING_OUTSHARES;
        }
        if (plink)
        {
            shareType |= ShareType_t::LINK;
        }
    }

    return shareType;
}

bool Node::isAncestor(NodeHandle ancestorHandle) const
{
    Node* ancestor = parent;
    while (ancestor)
    {
        if (ancestor->nodeHandle() == ancestorHandle)
        {
            return true;
        }

        ancestor = ancestor->parent;
    }

    return false;
}

#ifdef ENABLE_SYNC

void Node::detach(const bool recreate)
{
    if (localnode)
    {
        localnode->detach(recreate);
    }
}

#endif // ENABLE_SYNC

void Node::setkeyfromjson(const char* k)
{
    if (keyApplied()) --client->mAppliedKeyNodeCount;
    JSON::copystring(&nodekeydata, k);
    if (keyApplied()) ++client->mAppliedKeyNodeCount;
    assert(client->mAppliedKeyNodeCount >= 0);
}

void Node::setUndecryptedKey(const std::string& undecryptedKey)
{
    nodekeydata = undecryptedKey;
}

// update node key and decrypt attributes
void Node::setkey(const byte* newkey)
{
    if (newkey)
    {
        if (keyApplied()) --client->mAppliedKeyNodeCount;
        nodekeydata.assign(reinterpret_cast<const char*>(newkey), (type == FILENODE) ? FILENODEKEYLENGTH : FOLDERNODEKEYLENGTH);
        if (keyApplied()) ++client->mAppliedKeyNodeCount;
        assert(client->mAppliedKeyNodeCount >= 0);
    }

    setattr();
}

// serialize node - nodes with pending or RSA keys are unsupported
bool Node::serialize(string* d)
{
    // do not serialize encrypted nodes
    if (attrstring)
    {
        LOG_debug << "Trying to serialize an encrypted node";

        //Last attempt to decrypt the node
        applykey();
        setattr();

        if (attrstring)
        {
            LOG_debug << "Serializing an encrypted node.";
        }
    }

    switch (type)
    {
        case FILENODE:
            if (!attrstring && (int)nodekeydata.size() != FILENODEKEYLENGTH)
            {
                return false;
            }
            break;

        case FOLDERNODE:
            if (!attrstring && (int)nodekeydata.size() != FOLDERNODEKEYLENGTH)
            {
                return false;
            }
            break;

        default:
            if (nodekeydata.size())
            {
                return false;
            }
    }

    unsigned short ll;
    short numshares;
    m_off_t s;

    s = type ? -type : size;

    d->append((char*)&s, sizeof s);

    d->append((char*)&nodehandle, MegaClient::NODEHANDLE);

    if (parenthandle != UNDEF)
    {
        d->append((char*)&parenthandle, MegaClient::NODEHANDLE);
    }
    else
    {
        d->append("\0\0\0\0\0", MegaClient::NODEHANDLE);
    }

    d->append((char*)&owner, MegaClient::USERHANDLE);

    // FIXME: use Serialize64
    time_t ts = 0;  // we don't want to break backward compatibility by changing the size (where m_time_t differs)
    d->append((char*)&ts, sizeof(ts));

    ts = (time_t)ctime;
    d->append((char*)&ts, sizeof(ts));

    if (attrstring)
    {
        auto length = 0u;

        if (type == FOLDERNODE)
        {
            length = FOLDERNODEKEYLENGTH;
        }
        else if (type == FILENODE)
        {
            length = FILENODEKEYLENGTH;
        }

        d->append(length, '\0');
    }
    else
    {
        d->append(nodekeydata);
    }

    if (type == FILENODE)
    {
        ll = static_cast<unsigned short>(fileattrstring.size() + 1);
        d->append((char*)&ll, sizeof ll);
        d->append(fileattrstring.c_str(), ll);
    }

    char isExported = plink ? 1 : 0;
    d->append((char*)&isExported, 1);

    char hasLinkCreationTs = plink ? 1 : 0;
    d->append((char*)&hasLinkCreationTs, 1);

    if (isExported && plink && plink->mAuthKey.size())
    {
        auto authKeySize = (char)plink->mAuthKey.size();
        d->append((char*)&authKeySize, sizeof(authKeySize));
        d->append(plink->mAuthKey.data(), authKeySize);
    }
    else
    {
        d->append("", 1);
    }

    d->append(1, static_cast<char>(!!attrstring));

    if (attrstring)
    {
        d->append(1, '\1');
    }

    // Use these bytes for extensions.
    d->append(4, '\0');

    if (inshare)
    {
        numshares = -1;
    }
    else
    {
        numshares = 0;
        if (outshares)
        {
            numshares = static_cast<short int>(numshares + outshares->size());
        }
        if (pendingshares)
        {
            numshares = static_cast<short int>(numshares + pendingshares->size());
        }
    }

    d->append((char*)&numshares, sizeof numshares);

    if (numshares)
    {
        d->append((char*)sharekey->key, SymmCipher::KEYLENGTH);

        if (inshare)
        {
            inshare->serialize(d);
        }
        else
        {
            if (outshares)
            {
                for (share_map::iterator it = outshares->begin(); it != outshares->end(); it++)
                {
                    it->second->serialize(d);
                }
            }
            if (pendingshares)
            {
                for (share_map::iterator it = pendingshares->begin(); it != pendingshares->end(); it++)
                {
                    it->second->serialize(d);
                }
            }
        }
    }

    attrs.serialize(d);

    if (isExported)
    {
        d->append((char*) &plink->ph, MegaClient::NODEHANDLE);
        d->append((char*) &plink->ets, sizeof(plink->ets));
        d->append((char*) &plink->takendown, sizeof(plink->takendown));
        if (hasLinkCreationTs)
        {
            d->append((char*) &plink->cts, sizeof(plink->cts));
        }
    }

    // Write data necessary to thaw encrypted nodes.
    if (attrstring)
    {
        // Write node key data.
        unsigned short length = (unsigned short)nodekeydata.size();
        d->append((char*)&length, sizeof(length));
        d->append(nodekeydata, 0, length);

        // Write attribute string data.
        length = (unsigned short)attrstring->size();
        d->append((char*)&length, sizeof(length));
        d->append(*attrstring, 0, length);
    }

    return true;
}

// decrypt attrstring and check magic number prefix
byte* Node::decryptattr(SymmCipher* key, const char* attrstring, size_t attrstrlen)
{
    if (attrstrlen)
    {
        int l = int(attrstrlen * 3 / 4 + 3);
        byte* buf = new byte[l];

        l = Base64::atob(attrstring, buf, l);

        if (!(l & (SymmCipher::BLOCKSIZE - 1)))
        {
            key->cbc_decrypt(buf, l);

            if (!memcmp(buf, "MEGA{\"", 6))
            {
                return buf;
            }
        }

        delete[] buf;
    }

    return NULL;
}

void Node::parseattr(byte *bufattr, AttrMap &attrs, m_off_t size, m_time_t &mtime , string &fileName, string &fingerprint, FileFingerprint &ffp)
{
    JSON json;
    nameid name;
    string *t;

    json.begin((char*)bufattr + 5);
    while ((name = json.getnameid()) != EOO && json.storeobject((t = &attrs.map[name])))
    {
        JSON::unescape(t);
    }

    attr_map::iterator it = attrs.map.find('n');   // filename
    if (it == attrs.map.end())
    {
        fileName = "CRYPTO_ERROR";
    }
    else if (it->second.empty())
    {
        fileName = "BLANK";
    }

    it = attrs.map.find('c');   // checksum
    if (it != attrs.map.end())
    {
        if (ffp.unserializefingerprint(&it->second))
        {
            ffp.size = size;
            mtime = ffp.mtime;

            char bsize[sizeof(size) + 1];
            int l = Serialize64::serialize((byte *)bsize, size);
            char *buf = new char[l * 4 / 3 + 4];
            char ssize = static_cast<char>('A' + Base64::btoa((const byte *)bsize, l, buf));

            string result(1, ssize);
            result.append(buf);
            result.append(it->second);
            delete [] buf;

            fingerprint = result;
        }
    }
}

// return temporary SymmCipher for this nodekey
SymmCipher* Node::nodecipher()
{
    return client->getRecycledTemporaryNodeCipher(&nodekeydata);
}

// decrypt attributes and build attribute hash
void Node::setattr()
{
    byte* buf;
    SymmCipher* cipher;

    if (attrstring && (cipher = nodecipher()) && (buf = decryptattr(cipher, attrstring->c_str(), attrstring->size())))
    {
        JSON json;
        nameid name;
        string* t;

        AttrMap oldAttrs(attrs);
        attrs.map.clear();
        json.begin((char*)buf + 5);

        while ((name = json.getnameid()) != EOO && json.storeobject((t = &attrs.map[name])))
        {
            JSON::unescape(t);

            if (name == 'n')
            {
                LocalPath::utf8_normalize(t);
            }
        }

        changed.name = attrs.hasDifferentValue('n', oldAttrs.map);
        changed.favourite = attrs.hasDifferentValue(AttrMap::string2nameid("fav"), oldAttrs.map);

        setfingerprint();

        delete[] buf;

        attrstring.reset();
    }
}

// if present, configure FileFingerprint from attributes
// otherwise, the file's fingerprint is derived from the file's mtime/size/key
void Node::setfingerprint()
{
    if (type == FILENODE && nodekeydata.size() >= sizeof crc)
    {
        client->mNodeManager.removeFingerprint(this);

        attr_map::iterator it = attrs.map.find('c');

        if (it != attrs.map.end())
        {
            if (!unserializefingerprint(&it->second))
            {
                LOG_warn << "Invalid fingerprint";
            }
        }

        // if we lack a valid FileFingerprint for this file, use file's key,
        // size and client timestamp instead
        if (!isvalid)
        {
            memcpy(crc.data(), nodekeydata.data(), sizeof crc);
            mtime = ctime;
        }

        mFingerPrintPosition = client->mNodeManager.insertFingerprint(this);
    }
}

bool Node::hasName(const string& name) const
{
    auto it = attrs.map.find('n');
    return it != attrs.map.end() && it->second == name;
}

// return file/folder name or special status strings
const char* Node::displayname() const
{
    // not yet decrypted
    if (attrstring)
    {
        LOG_debug << "NO_KEY " << type << " " << size << " " << Base64Str<MegaClient::NODEHANDLE>(nodehandle);
#ifdef ENABLE_SYNC
        if (localnode)
        {
            LOG_debug << "Local name: " << localnode->name;
        }
#endif
        return "NO_KEY";
    }

    attr_map::const_iterator it;

    it = attrs.map.find('n');

    if (it == attrs.map.end())
    {
        if (type < ROOTNODE || type > RUBBISHNODE)
        {
            LOG_debug << "CRYPTO_ERROR " << type << " " << size << " " << nodehandle;
#ifdef ENABLE_SYNC
            if (localnode)
            {
                LOG_debug << "Local name: " << localnode->name;
            }
#endif
        }
        return "CRYPTO_ERROR";
    }

    if (!it->second.size())
    {
        LOG_debug << "BLANK " << type << " " << size << " " << nodehandle;
#ifdef ENABLE_SYNC
        if (localnode)
        {
            LOG_debug << "Local name: " << localnode->name;
        }
#endif
        return "BLANK";
    }

    return it->second.c_str();
}

string Node::displaypath() const
{
    // factored from nearly identical functions in megapi_impl and megacli
    string path;
    const Node* n = this;
    for (; n; n = n->parent)
    {
        switch (n->type)
        {
        case FOLDERNODE:
            path.insert(0, n->displayname());

            if (n->inshare)
            {
                path.insert(0, ":");
                if (n->inshare->user)
                {
                    path.insert(0, n->inshare->user->email);
                }
                else
                {
                    path.insert(0, "UNKNOWN");
                }
                return path;
            }
            break;

        case VAULTNODE:
            path.insert(0, "//in");
            return path;

        case ROOTNODE:
            return path.empty() ? "/" : path;

        case RUBBISHNODE:
            path.insert(0, "//bin");
            return path;

        case TYPE_DONOTSYNC:
        case TYPE_SPECIAL:
        case TYPE_UNKNOWN:
        case FILENODE:
            path.insert(0, n->displayname());
        }
        path.insert(0, "/");
    }
    return path;
}

// returns position of file attribute or 0 if not present
int Node::hasfileattribute(fatype t) const
{
    return Node::hasfileattribute(&fileattrstring, t);
}

int Node::hasfileattribute(const string *fileattrstring, fatype t)
{
    char buf[24];

    sprintf(buf, ":%u*", t);
    return static_cast<int>(fileattrstring->find(buf) + 1);
}

// attempt to apply node key - sets nodekey to a raw key if successful
bool Node::applykey()
{
    if (type > FOLDERNODE)
    {
        //Root nodes contain an empty attrstring
        attrstring.reset();
    }

    if (keyApplied() || !nodekeydata.size())
    {
        return false;
    }

    int l = -1;
    size_t t = 0;
    handle h;
    const char* k = NULL;
    SymmCipher* sc = &client->key;
    handle me = client->loggedin() ? client->me : client->mNodeManager.getRootNodeFiles().as8byte();

    while ((t = nodekeydata.find_first_of(':', t)) != string::npos)
    {
        // compound key: locate suitable subkey (always symmetric)
        h = 0;

        l = Base64::atob(nodekeydata.c_str() + (nodekeydata.find_last_of('/', t) + 1), (byte*)&h, sizeof h);
        t++;

        if (l == MegaClient::USERHANDLE)
        {
            // this is a user handle - reject if it's not me
            if (h != me)
            {
                continue;
            }
        }
        else
        {
            // look for share key if not folder access with folder master key
            if (h != me)
            {
                // this is a share node handle - check if share key is available at key's repository
                // if not available, check if the node already has the share key
                auto it = client->mNewKeyRepository.find(NodeHandle().set6byte(h));
                if (it == client->mNewKeyRepository.end())
                {
                    Node* n;
                    if (!(n = client->nodebyhandle(h)) || !n->sharekey)
                    {
                        continue;
                    }

                    sc = n->sharekey;
                }
                else
                {
                    sc = it->second.get();
                }

                // this key will be rewritten when the node leaves the outbound share
                foreignkey = true;
            }
        }

        k = nodekeydata.c_str() + t;
        break;
    }

    // no: found => personal key, use directly
    // otherwise, no suitable key available yet - bail (it might arrive soon)
    if (!k)
    {
        if (l < 0)
        {
            k = nodekeydata.c_str();
        }
        else
        {
            return false;
        }
    }

    byte key[FILENODEKEYLENGTH];
    unsigned keylength = (type == FILENODE) ? FILENODEKEYLENGTH : FOLDERNODEKEYLENGTH;

    if (client->decryptkey(k, key, keylength, sc, 0, nodehandle))
    {
        client->mAppliedKeyNodeCount++;
        nodekeydata.assign((const char*)key, keylength);
        setattr();
    }

    bool applied = keyApplied();
    if (!applied)
    {
        LOG_warn << "Failed to apply key for node: " << Base64Str<MegaClient::NODEHANDLE>(nodehandle);
        // keys could be missing due to nested inshares with multiple users: user A shares a folder 1
        // with user B and folder 1 has a subfolder folder 1_1. User A shares folder 1_1 with user C
        // and user C adds some files, which will be undecryptable for user B.
        // The ticket SDK-1959 aims to mitigate the problem. Uncomment next line when done:
        // assert(applied);
    }

    return applied;
}

NodeCounter Node::getCounter() const
{
    return mCounter;
}

void Node::setCounter(const NodeCounter &counter, bool notify)
{
    mCounter = counter;

    if (notify)
    {
        changed.counter = true;
        client->notifynode(this);
    }
}

// returns whether node was moved
bool Node::setparent(Node* p, bool updateNodeCounters)
{
    if (p == parent)
    {
        return false;
    }

    Node *oldparent = parent;
    if (oldparent)
    {
        client->mNodeManager.removeChild(oldparent, nodeHandle());
    }

    parenthandle = p ? p->nodehandle : UNDEF;
    parent = p;
    if (parent)
    {
        client->mNodeManager.addChild(parent->nodeHandle(), nodeHandle(), this);
    }

    if (updateNodeCounters)
    {
        client->mNodeManager.updateCounter(*this, oldparent);
    }

#ifdef ENABLE_SYNC
    // 'updateNodeCounters' is false when node is loaded from DB. In that case, we want to skip the
    // processing by TreeProcDelSyncGet, since the node won't have a valid SyncFileGet yet.
    // (this is important, since otherwise the whole tree beneath this node will be loaded in
    // result of the proctree())
    if (updateNodeCounters)
    {
        // if we are moving an entire sync, don't cancel GET transfers
        if (!localnode || localnode->parent)
        {
            // if the new location is not synced, cancel all GET transfers
            while (p)
            {
                if (p->localnode)
                {
                    break;
                }

                p = p->parent;
            }

            if (!p || p->type == FILENODE)
            {
                DBTableTransactionCommitter committer(client->tctable); // potentially stopping many transfers here
                TreeProcDelSyncGet tdsg;
                client->proctree(this, &tdsg);
            }
        }
    }

    if (oldparent && oldparent->localnode)
    {
        oldparent->localnode->treestate(oldparent->localnode->checkstate());
    }
#endif

    return true;
}

const Node* Node::firstancestor() const
{
    const Node* n = this;
    while (n->parent != NULL)
    {
        n = n->parent;
    }

    return n;
}

const Node* Node::latestFileVersion() const
{
    const Node* n = this;
    if (type == FILENODE)
    {
        while (n->parent && n->parent->type == FILENODE)
        {
            n = n->parent;
        }
    }
    return n;
}

// returns 1 if n is under p, 0 otherwise
bool Node::isbelow(Node* p) const
{
    const Node* n = this;

    for (;;)
    {
        if (!n)
        {
            return false;
        }

        if (n == p)
        {
            return true;
        }

        n = n->parent;
    }
}

bool Node::isbelow(NodeHandle p) const
{
    const Node* n = this;

    for (;;)
    {
        if (!n)
        {
            return false;
        }

        if (n->nodeHandle() == p)
        {
            return true;
        }

        n = n->parent;
    }
}

void Node::setpubliclink(handle ph, m_time_t cts, m_time_t ets, bool takendown, const string &authKey)
{
    if (!plink) // creation
    {
        plink = new PublicLink(ph, cts, ets, takendown, authKey.empty() ? nullptr : authKey.c_str());
    }
    else            // update
    {
        plink->ph = ph;
        plink->cts = cts;
        plink->ets = ets;
        plink->takendown = takendown;
        plink->mAuthKey = authKey;
    }
}

PublicLink::PublicLink(handle ph, m_time_t cts, m_time_t ets, bool takendown, const char *authKey)
{
    this->ph = ph;
    this->cts = cts;
    this->ets = ets;
    this->takendown = takendown;
    if (authKey)
    {
        this->mAuthKey = authKey;
    }
}

PublicLink::PublicLink(PublicLink *plink)
{
    this->ph = plink->ph;
    this->cts = plink->cts;
    this->ets = plink->ets;
    this->takendown = plink->takendown;
    this->mAuthKey = plink->mAuthKey;
}

bool PublicLink::isExpired()
{
    if (!ets)       // permanent link: ets=0
        return false;

    m_time_t t = m_time();
    return ets < t;
}

#ifdef ENABLE_SYNC
// set, change or remove LocalNode's parent and name/localname/slocalname.
// newlocalpath must be a full path and must not point to an empty string.
// no shortname allowed as the last path component.
void LocalNode::setnameparent(LocalNode* newparent, const LocalPath* newlocalpath, std::unique_ptr<LocalPath> newshortname)
{
    if (!sync)
    {
        LOG_err << "LocalNode::init() was never called";
        assert(false);
        return;
    }

    bool newnode = localname.empty();
    Node* todelete = NULL;
    int nc = 0;
    Sync* oldsync = NULL;

    assert(!newparent || newparent->node || newnode);

    if (parent)
    {
        // remove existing child linkage
        parent->children.erase(&localname);

        if (slocalname)
        {
            parent->schildren.erase(slocalname.get());
            slocalname.reset();
        }
    }

    if (newlocalpath)
    {
        // extract name component from localpath, check for rename unless newnode
        size_t p = newlocalpath->getLeafnameByteIndex();

        // has the name changed?
        if (!newlocalpath->backEqual(p, localname))
        {
            // set new name
            localname = newlocalpath->subpathFrom(p);
            name = localname.toName(*sync->client->fsaccess);

            if (node)
            {
                if (name != node->attrs.map['n'])
                {
                    if (node->type == FILENODE)
                    {
                        treestate(TREESTATE_SYNCING);
                    }
                    else
                    {
                        sync->client->app->syncupdate_treestate(sync->getConfig(), getLocalPath(), ts, type);
                    }

                    string prevname = node->attrs.map['n'];

                    // set new name
                    sync->client->setattr(node, attr_map('n', name), sync->client->nextreqtag(), prevname.c_str(), nullptr);
                }
            }
        }
    }

    if (parent && parent != newparent && !sync->mDestructorRunning)
    {
        treestate(TREESTATE_NONE);
    }

    if (newparent)
    {
        if (newparent != parent)
        {
            parent = newparent;

            if (!newnode && node)
            {
                sync->client->nextreqtag(); //make reqtag advance to use the next one
                LOG_debug << "Moving node: " << node->displaypath() << " to " << parent->node->displaypath();
                if (sync->client->rename(node, parent->node, SYNCDEL_NONE, node->parent ? node->parent->nodeHandle() : NodeHandle(), nullptr, nullptr) == API_EACCESS
                        && sync != parent->sync)
                {
                    LOG_debug << "Rename not permitted. Using node copy/delete";

                    // save for deletion
                    todelete = node;
                }

                if (type == FILENODE)
                {
                    ts = TREESTATE_SYNCING;
                }
            }

            if (sync != parent->sync)
            {
                LOG_debug << "Moving files between different syncs";
                oldsync = sync;
            }

            if (todelete || oldsync)
            {
                // prepare localnodes for a sync change or/and a copy operation
                LocalTreeProcMove tp(parent->sync, todelete != NULL);
                sync->client->proclocaltree(this, &tp);
                nc = tp.nc;
            }
        }

        // (we don't construct a UTF-8 or sname for the root path)
        parent->children[&localname] = this;

        if (newshortname && *newshortname != localname)
        {
            slocalname = std::move(newshortname);
            parent->schildren[slocalname.get()] = this;
        }
        else
        {
            slocalname.reset();
        }

        treestate(TREESTATE_NONE);

        if (todelete)
        {
            // complete the copy/delete operation
            dstime nds = NEVER;
            sync->client->syncup(parent, &nds);

            // check if nodes can be immediately created
            bool immediatecreation = (int) sync->client->synccreate.size() == nc;

            sync->client->syncupdate();

            // try to keep nodes in syncdebris if they can't be immediately created
            // to avoid uploads
            sync->client->movetosyncdebris(todelete, immediatecreation || oldsync->inshare);
        }

        if (oldsync)
        {
            // update local cache if there is a sync change
            oldsync->cachenodes();
            sync->cachenodes();
        }
    }

    if (newlocalpath)
    {
        LocalTreeProcUpdateTransfers tput;
        sync->client->proclocaltree(this, &tput);
    }
}

// delay uploads by 1.1 s to prevent server flooding while a file is still being written
void LocalNode::bumpnagleds()
{
    if (!sync)
    {
        LOG_err << "LocalNode::init() was never called";
        assert(false);
        return;
    }

    nagleds = sync->client->waiter->ds + 11;
}

LocalNode::LocalNode(Sync* csync)
: sync(csync)
, deleted{false}
, created{false}
, reported{false}
, checked{false}
, needsRescan(false)
{}

// initialize fresh LocalNode object - must be called exactly once
void LocalNode::init(nodetype_t ctype, LocalNode* cparent, const LocalPath& cfullpath, std::unique_ptr<LocalPath> shortname)
{
    parent = NULL;
    node.reset();
    notseen = 0;
    deleted = false;
    created = false;
    reported = false;
    needsRescan = false;
    syncxfer = true;
    newnode.reset();
    parent_dbid = 0;
    slocalname = NULL;

    ts = TREESTATE_NONE;
    dts = TREESTATE_NONE;

    type = ctype;
    syncid = sync->client->nextsyncid();

    bumpnagleds();

    if (cparent)
    {
        setnameparent(cparent, &cfullpath, std::move(shortname));
    }
    else
    {
        localname = cfullpath;
        slocalname.reset(shortname && *shortname != localname ? shortname.release() : nullptr);
        name = localname.toPath();
    }

    scanseqno = sync->scanseqno;

    // mark fsid as not valid
    fsid_it = sync->client->fsidnode.end();

    // enable folder notification
    if (type == FOLDERNODE && sync->dirnotify)
    {
        sync->dirnotify->addnotify(this, cfullpath);
    }

    sync->client->syncactivity = true;

    sync->client->totalLocalNodes++;
    sync->localnodes[type]++;
}

// update treestates back to the root LocalNode, inform app about changes
void LocalNode::treestate(treestate_t newts)
{
    if (!sync)
    {
        LOG_err << "LocalNode::init() was never called";
        assert(false);
        return;
    }

    if (newts != TREESTATE_NONE)
    {
        ts = newts;
    }

    if (ts != dts)
    {
        sync->client->app->syncupdate_treestate(sync->getConfig(), getLocalPath(), ts, type);
    }

    if (parent && ((newts == TREESTATE_NONE && ts != TREESTATE_NONE)
                   || (ts != dts && (!(ts == TREESTATE_SYNCED && parent->ts == TREESTATE_SYNCED))
                                 && (!(ts == TREESTATE_SYNCING && parent->ts == TREESTATE_SYNCING))
                                 && (!(ts == TREESTATE_PENDING && (parent->ts == TREESTATE_PENDING
                                                                   || parent->ts == TREESTATE_SYNCING))))))
    {
        treestate_t state = TREESTATE_NONE;
        if (newts != TREESTATE_NONE && ts == TREESTATE_SYNCING)
        {
            state = TREESTATE_SYNCING;
        }
        else
        {
            state = parent->checkstate();
        }

        parent->treestate(state);
    }

    dts = ts;
}

treestate_t LocalNode::checkstate()
{
    if (type == FILENODE)
        return ts;

    treestate_t state = TREESTATE_SYNCED;
    for (localnode_map::iterator it = children.begin(); it != children.end(); it++)
    {
        if (it->second->ts == TREESTATE_SYNCING)
        {
            state = TREESTATE_SYNCING;
            break;
        }

        if (it->second->ts == TREESTATE_PENDING && state == TREESTATE_SYNCED)
        {
            state = TREESTATE_PENDING;
        }
    }
    return state;
}

void LocalNode::setnode(Node* cnode)
{
    deleted = false;

    node.reset();
    if (cnode)
    {
        cnode->localnode.reset();
        node.crossref(cnode, this);
    }
}

void LocalNode::setnotseen(int newnotseen)
{
    if (!sync)
    {
        LOG_err << "LocalNode::init() was never called";
        assert(false);
        return;
    }

    if (!newnotseen)
    {
        if (notseen)
        {
            sync->client->localsyncnotseen.erase(notseen_it);
        }

        notseen = 0;
        scanseqno = sync->scanseqno;
    }
    else
    {
        if (!notseen)
        {
            notseen_it = sync->client->localsyncnotseen.insert(this).first;
        }

        notseen = newnotseen;
    }
}

// set fsid - assume that an existing assignment of the same fsid is no longer current and revoke
void LocalNode::setfsid(handle newfsid, handlelocalnode_map& fsidnodes)
{
    if (!sync)
    {
        LOG_err << "LocalNode::init() was never called";
        assert(false);
        return;
    }

    if (fsid_it != fsidnodes.end())
    {
        if (newfsid == fsid)
        {
            return;
        }

        fsidnodes.erase(fsid_it);
    }

    fsid = newfsid;

    pair<handlelocalnode_map::iterator, bool> r = fsidnodes.insert(std::make_pair(fsid, this));

    fsid_it = r.first;

    if (!r.second)
    {
        // remove previous fsid assignment (the node is likely about to be deleted)
        fsid_it->second->fsid_it = fsidnodes.end();
        fsid_it->second = this;
    }
}

LocalNode::~LocalNode()
{
    if (!sync)
    {
        LOG_err << "LocalNode::init() was never called";
        assert(false);
        return;
    }

    if (!sync->mDestructorRunning && (
        sync->state() == SYNC_ACTIVE || sync->state() == SYNC_INITIALSCAN))
    {
        sync->statecachedel(this);

        if (type == FOLDERNODE)
        {
            LOG_debug << "Sync - local folder deletion detected: " << getLocalPath().toPath();
        }
        else
        {
            LOG_debug << "Sync - local file deletion detected: " << getLocalPath().toPath();
        }
    }

    setnotseen(0);

    newnode.reset();

    if (sync->dirnotify.get())
    {
        // deactivate corresponding notifyq records
        for (int q = DirNotify::RETRY; q >= DirNotify::EXTRA; q--)
        {
            sync->dirnotify->notifyq[q].replaceLocalNodePointers(this, (LocalNode*)~0);
        }
    }

    // remove from fsidnode map, if present
    if (fsid_it != sync->client->fsidnode.end())
    {
        sync->client->fsidnode.erase(fsid_it);
    }

    sync->client->totalLocalNodes--;
    sync->localnodes[type]--;

    if (type == FILENODE && size > 0)
    {
        sync->localbytes -= size;
    }

    if (type == FOLDERNODE)
    {
        if (sync->dirnotify.get())
        {
            sync->dirnotify->delnotify(this);
        }
    }

    // remove parent association
    if (parent)
    {
        setnameparent(NULL, NULL, NULL);
    }

    for (localnode_map::iterator it = children.begin(); it != children.end(); )
    {
        delete it++->second;
    }

    if (node && !sync->mDestructorRunning)
    {
        // move associated node to SyncDebris unless the sync is currently
        // shutting down
        if (sync->state() >= SYNC_INITIALSCAN)
        {
            sync->client->movetosyncdebris(node, sync->inshare);
        }
    }
}

void LocalNode::detach(const bool recreate)
{
    // Never detach the sync root.
    if (parent && node)
    {
        node.reset();
        created &= !recreate;
    }
}

void LocalNode::setSubtreeNeedsRescan(bool includeFiles)
{
    assert(type != FILENODE);

    needsRescan = true;

    for (auto& child : children)
    {
        if (child.second->type != FILENODE)
        {
            child.second->setSubtreeNeedsRescan(includeFiles);
        }
        else
        {
            child.second->needsRescan |= includeFiles;
        }
    }
}

LocalPath LocalNode::getLocalPath() const
{
    LocalPath lp;
    getlocalpath(lp);
    return lp;
}

void LocalNode::getlocalpath(LocalPath& path) const
{
    if (!sync)
    {
        LOG_err << "LocalNode::init() was never called";
        assert(false);
        return;
    }

    path.clear();

    for (const LocalNode* l = this; l != nullptr; l = l->parent)
    {
        assert(!l->parent || l->parent->sync == sync);

        // sync root has absolute path, the rest are just their leafname
        path.prependWithSeparator(l->localname);
    }
}

// locate child by localname or slocalname
LocalNode* LocalNode::childbyname(LocalPath* localname)
{
    localnode_map::iterator it;

    if (!localname || ((it = children.find(localname)) == children.end() && (it = schildren.find(localname)) == schildren.end()))
    {
        return NULL;
    }

    return it->second;
}

void LocalNode::prepare(FileSystemAccess&)
{
    getlocalpath(transfer->localfilename);
    assert(transfer->localfilename.isAbsolute());


    // is this transfer in progress? update file's filename.
    if (transfer->slot && transfer->slot->fa && !transfer->slot->fa->nonblocking_localname.empty())
    {
        transfer->slot->fa->updatelocalname(transfer->localfilename, false);
    }

    treestate(TREESTATE_SYNCING);
}

void LocalNode::terminated(error e)
{
    sync->threadSafeState->transferComplete(PUT, size);

    File::terminated(e);
}

// complete a sync upload: complete to //bin if a newer node exists (which
// would have been caused by a race condition)
void LocalNode::completed(Transfer* t, putsource_t source)
{
    sync->threadSafeState->transferFailed(PUT, size);

    // complete to rubbish for later retrieval if the parent node does not
    // exist or is newer
    if (!parent || !parent->node || (node && mtime < node->mtime))
    {
        h = t->client->mNodeManager.getRootNodeRubbish();
    }
    else
    {
        // otherwise, overwrite node if it already exists and complete in its
        // place
        h = parent->node->nodeHandle();
    }

    // we are overriding completed() for sync upload, we don't use the File::completed version at all.
    assert(t->type == PUT);
    sendPutnodes(t->client, t->uploadhandle, *t->ultoken, t->filekey, source, NodeHandle(), nullptr, this, nullptr);
}

// serialize/unserialize the following LocalNode properties:
// - type/size
// - fsid
// - parent LocalNode's dbid
// - corresponding Node handle
// - local name
// - fingerprint crc/mtime (filenodes only)
bool LocalNode::serialize(string* d)
{
    CacheableWriter w(*d);
    w.serializei64(type ? -type : size);
    w.serializehandle(fsid);
    w.serializeu32(parent ? parent->dbid : 0);
    w.serializenodehandle(node ? node->nodehandle : UNDEF);
    w.serializestring(localname.platformEncoded());
    if (type == FILENODE)
    {
        w.serializebinary((byte*)crc.data(), sizeof(crc));
        w.serializecompressed64(mtime);
    }
    w.serializebyte(mSyncable);
    w.serializeexpansionflags(1);  // first flag indicates we are storing slocalname.  Storing it is much, much faster than looking it up on startup.
    auto tmpstr = slocalname ? slocalname->platformEncoded() : string();
    w.serializepstr(slocalname ? &tmpstr : nullptr);

    return true;
}

LocalNode* LocalNode::unserialize(Sync* sync, const string* d)
{
    if (d->size() < sizeof(m_off_t)         // type/size combo
                  + sizeof(handle)          // fsid
                  + sizeof(uint32_t)        // parent dbid
                  + MegaClient::NODEHANDLE  // handle
                  + sizeof(short))          // localname length
    {
        LOG_err << "LocalNode unserialization failed - short data";
        return NULL;
    }

    CacheableReader r(*d);

    nodetype_t type;
    m_off_t size;

    if (!r.unserializei64(size)) return nullptr;

    if (size < 0 && size >= -FOLDERNODE)
    {
        // will any compiler optimize this to a const assignment?
        type = (nodetype_t)-size;
        size = 0;
    }
    else
    {
        type = FILENODE;
    }

    handle fsid;
    uint32_t parent_dbid;
    handle h = 0;
    string localname, shortname;
    uint64_t mtime = 0;
    int32_t crc[4];
    memset(crc, 0, sizeof crc);
    byte syncable = 1;
    unsigned char expansionflags[8] = { 0 };

    if (!r.unserializehandle(fsid) ||
        !r.unserializeu32(parent_dbid) ||
        !r.unserializenodehandle(h) ||
        !r.unserializestring(localname) ||
        (type == FILENODE && !r.unserializebinary((byte*)crc, sizeof(crc))) ||
        (type == FILENODE && !r.unserializecompressed64(mtime)) ||
        (r.hasdataleft() && !r.unserializebyte(syncable)) ||
        (r.hasdataleft() && !r.unserializeexpansionflags(expansionflags, 1)) ||
        (expansionflags[0] && !r.unserializecstr(shortname, false)))
    {
        LOG_err << "LocalNode unserialization failed at field " << r.fieldnum;
        return nullptr;
    }
    assert(!r.hasdataleft());

    LocalNode* l = new LocalNode(sync);

    l->type = type;
    l->size = size;

    l->parent_dbid = parent_dbid;

    l->fsid = fsid;
    l->fsid_it = sync->client->fsidnode.end();

    l->localname = LocalPath::fromPlatformEncodedRelative(localname);
    l->slocalname.reset(shortname.empty() ? nullptr : new LocalPath(LocalPath::fromPlatformEncodedRelative(shortname)));
    l->slocalname_in_db = 0 != expansionflags[0];
    l->name = l->localname.toName(*sync->client->fsaccess);

    memcpy(l->crc.data(), crc, sizeof crc);
    l->mtime = mtime;
    l->isvalid = true;

    l->node.store_unchecked(sync->client->nodebyhandle(h));
    l->parent = nullptr;
    l->sync = sync;
    l->mSyncable = syncable == 1;

    // FIXME: serialize/unserialize
    l->created = false;
    l->reported = false;
    l->checked = h != UNDEF; // TODO: Is this a bug? h will never be UNDEF
    l->needsRescan = false;

    return l;
}

#endif

void NodeCounter::operator += (const NodeCounter& o)
{
    storage += o.storage;
    files += o.files;
    folders += o.folders;
    versions += o.versions;
    versionStorage += o.versionStorage;
}

void NodeCounter::operator -= (const NodeCounter& o)
{
    storage -= o.storage;
    files -= o.files;
    folders -= o.folders;
    versions -= o.versions;
    versionStorage -= o.versionStorage;
}

std::string NodeCounter::serialize() const
{
    std::string nodeCountersBlob;
    CacheableWriter w(nodeCountersBlob);
    w.serializesize_t(files);
    w.serializesize_t(folders);
    w.serializei64(storage);
    w.serializesize_t(versions);
    w.serializei64(versionStorage);

    return nodeCountersBlob;
}

NodeCounter::NodeCounter(const std::string &blob)
{
    CacheableReader r(blob);
    r.unserializesize_t(files);
    r.unserializesize_t(folders);
    r.unserializei64(storage);
    r.unserializesize_t(versions);
    r.unserializei64(versionStorage);
}

} // namespace
