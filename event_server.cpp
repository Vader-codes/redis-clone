#include <stdio.h>      // for prinf
#include <unistd.h>     // for close
#include <sys/socket.h> // for socket, bind, listen, accept
#include <stdlib.h>     // for exit
#include <arpa/inet.h>  // sockaddr_in, htons, htonl
#include <string.h>     // memcpy, strlen
#include <errno.h>      //errno
#include <fcntl.h>      // fnctl, O_NONBlock
#include <vector>       // vector in c++
#include <poll.h>
#include <string>
#include <assert.h>
#include <stddef.h> // for offsetof
using namespace std;

// maximun message our server can receive (32MB)
const size_t k_max_msg = 32 << 20; // 32* 2^20

const size_t k_rehashing_work = 128; // migrate at most this many per call

const size_t k_max_load_factor = 8; // max keys per slot before resizing

// response type tags (TLV format)
const uint8_t TAG_NIL = 0; // nothing
const uint8_t TAG_ERR = 1; // error
const uint8_t TAG_STR = 2; // string
const uint8_t TAG_INT = 3; // 64-bit integer
const uint8_t TAG_DBL = 4; // 64-bit double (unused for now)
const uint8_t TAG_ARR = 5; // array

const uint32_t ERR_UNKNOWN = 1; // unknown command
const uint32_t ERR_BAD_ARG = 2; // wrong arguments

// given a pointer to a member, compute the address of its containing struct
#define container_of(ptr, T, member) \
    ((T *)((char *)(ptr) - offsetof(T, member)))

// growable byte buffer with 0(1) consume from the front
struct Buffer
{
    uint8_t *buffer_begin = NULL; // start of allocated memory
    uint8_t *buffer_end = NULL;   // one past end of allocation
    uint8_t *data_begin = NULL;   // first unread byte
    uint8_t *data_end = NULL;     // one past unread byte
};

// consume n bytes from the front (0 (1)) - just moves a pointer
static void buf_consume(Buffer *buf, size_t n)
{
    buf->data_begin += n;

    if (buf->data_begin == buf->data_end)
    {
        // buffer is now empty - reset both to the start
        // reclaims all space at once
        buf->data_begin = buf->buffer_begin;
        buf->data_end = buf->buffer_begin;
    }
}

// append len bytes to the buffer , growing or compacting as needed
static void buf_append(Buffer *buf, const uint8_t *data, size_t len)
{
    // first append : allocate initial block
    if (buf->buffer_begin == NULL)
    {
        size_t init_size = (len < 64) ? 64 : len; // at least 64bytes
        buf->buffer_begin = (uint8_t *)malloc(init_size);
        buf->buffer_end = buf->buffer_begin + init_size;
        buf->data_begin = buf->buffer_begin;
        buf->data_end = buf->buffer_begin;
    }
    // we have already allocated space, check how much is left
    size_t back_room = (size_t)(buf->buffer_end - buf->data_end);
    if (back_room < len)
    {
        // not enough room at the back - try compacting
        size_t data_size = (size_t)(buf->data_end - buf->data_begin);

        if (buf->data_begin > buf->buffer_begin)
        {
            // there's used space at the front  - move data back to the start
            memmove(buf->buffer_begin, buf->data_begin, data_size);
            buf->data_begin = buf->buffer_begin;
            buf->data_end = buf->buffer_begin + data_size;

            // recompute the backroom
            back_room = (size_t)(buf->data_end - buf->buffer_end);
        }

        if (back_room < len)
        {
            // still not enough reallocate double the size
            size_t old_size = (size_t)(buf->buffer_end - buf->buffer_begin);
            size_t new_size = old_size * 2;

            if (new_size < old_size + len)
            {
                new_size = old_size + len; // make sure it fits
            }
            uint8_t *new_block = (uint8_t *)malloc(new_size);
            memcpy(new_block, buf->data_begin, data_size); // recopy the previous data to new block
            free(buf->buffer_begin);                       // free the old data

            buf->buffer_begin = new_block;
            buf->buffer_end = new_block + new_size;
            buf->data_begin = new_block;
            buf->data_end = new_block + data_size;
        }
    }
    // now there's definetely enough space - copy the new data
    memcpy(buf->data_end, data, len);
    buf->data_end += len;
}

// append a single byte into the buffer
static void buf_append_u8(Buffer *out, uint8_t byte)
{
    buf_append(out, &byte, 1);
}

// append a 4-byte uint32_t
static void buf_append_u32(Buffer *out, uint32_t val)
{
    buf_append(out, (uint8_t *)&val, 4);
}

// append an 8-byte int64_t
static void buf_append_i64(Buffer *out, int64_t val)
{
    buf_append(out, (uint8_t *)&val, 8);
}

// free the buffer's memory
static void free_buffer(Buffer *buf)
{
    free(buf->buffer_begin);

    buf->buffer_begin = buf->buffer_end = NULL;
    buf->data_begin = buf->data_end = NULL;
}
// how many unread bytes are currently in the buffer?
static size_t buf_data_size(const Buffer *buf)
{
    return (size_t)(buf->data_end - buf->data_begin);
}

// pointer to the first unread byte
static uint8_t *buf_data(const Buffer *buf)
{
    return buf->data_begin;
}

// FNV-1a hash — fast, simple, good distribution for hashtables
static uint64_t str_hash(const uint8_t *data, size_t len)
{
    uint32_t h = 2166136261u; // FNV offset basis
    for (size_t i = 0; i < len; i++)
    {
        h = (h + data[i]) * 16777619; // FNV prime
    }
    return h;
}

// intrusive AVL tree node
struct AVLNode
{
    AVLNode *parent = NULL; // parent node (for rank queries)
    AVLNode *left = NULL;   // left child (smaller)
    AVLNode *right = NULL;  // right child (bigger)
    uint32_t height = 1;
    uint32_t cnt = 1; // subtree size , for rank queries
};

// height of a subtree (0 for empty)
static uint32_t avl_height(AVLNode *node)
{
    return node ? node->height : 0;
}

// subtree size (0 for empty)
static uint32_t avl_cnt(AVLNode *node)
{
    return node ? node->cnt : 0;
}

// recompute a node's height and cnt from its children
static void avl_update(AVLNode *node)
{
    node->height = 1 + max(avl_height(node->left), avl_height(node->right));
    node->cnt = 1 + avl_cnt(node->left) + avl_cnt(node->right);
}

// rotate node's right child up (fixes right-heavy)
static AVLNode *rot_left(AVLNode *node)
{
    AVLNode *parent = node->parent; // save old parent
    AVLNode *new_top = node->right; // D becomes the new top
    AVLNode *inner = new_top->left; // C (moves to node's right)

    // node.right = inner
    node->right = inner;
    if (inner)
        inner->parent = node;

    // new_top.parent = old parent
    new_top->parent = parent;

    // new_top.left = node
    new_top->left = node;
    node->parent = new_top;

    // update heights (bottom-up)
    avl_update(node);
    avl_update(new_top);

    return new_top; // caller links this to old parent
}

// rotate node's left child up (fixes left-heavy)
static AVLNode *rot_right(AVLNode *node)
{
    AVLNode *parent = node->parent;
    AVLNode *new_top = node->left;
    AVLNode *inner = new_top->right;

    node->left = inner;
    if (inner)
        inner->parent = node;

    new_top->parent = parent;

    new_top->right = node;
    node->parent = new_top;

    avl_update(node);
    avl_update(new_top);

    return new_top;
}

// fix a left-heavy node: rotation(s) to restore balance
static AVLNode *avl_fix_left(AVLNode *node)
{
    // if inner subtree is taller, rotate left first (LR case)
    if (avl_height(node->left->left) < avl_height(node->left->right))
    {
        node->left = rot_left(node->left);
    }
    return rot_right(node);
}

// fix a right-heavy node: rotation(s) to restore balance
static AVLNode *avl_fix_right(AVLNode *node)
{
    if (avl_height(node->right->right) < avl_height(node->right->left))
    {
        node->right = rot_right(node->right);
    }
    return rot_left(node);
}

// walk up from `node`, update heights, fix imbalances.
// returns the new root of the whole tree.
static AVLNode *avl_fix(AVLNode *node)
{
    while (true)
    {
        AVLNode **from = &node; // where this subtree links from
        AVLNode *parent = node->parent;
        if (parent)
        { // if not root
            from = parent->left == node ? &parent->left : &parent->right;
        }

        avl_update(node); // recompute height

        // check balance
        uint32_t l = avl_height(node->left);
        uint32_t r = avl_height(node->right);
        if (l == r + 2)
        {
            *from = avl_fix_left(node);
        }
        else if (l + 2 == r)
        {
            *from = avl_fix_right(node);
        }

        if (!parent)
            return *from; // reached root — done
        node = parent;    // continue upward
    }
}

// intrusive hashtable node - embedded inside the data it indexes
struct HNode
{
    HNode *next = nullptr; // linked list pointer for chaining
    uint64_t hcode = 0;    // cached hash value of the key
};

// a fixed - size hashtable (array of chain head)
struct HTab
{
    HNode **tab = NULL; // array of slots ;each slot is a pointer to a chain head
    size_t mask = 0;    // array size -1 capacity of our hash table
    size_t size = 0;    // numbers of keys currently stored
};

// one key-value pair with an embedded hashtable node
struct Entry
{
    HNode node; // intrusive node (must be first for cleaner code)
    string key; // the key
    string val; // the value
};

// initialize the hashtable with n slots (n must be a power of 2)
static void h_init(HTab *htab, size_t n)
{
    assert(n > 0 && ((n - 1) & n) == 0); // n must be a power of 2

    htab->tab = (HNode **)calloc(n, sizeof(HNode *)); // zeroed array
    htab->mask = n - 1;
    htab->size = 0;
}

// insert a node into the hashtable
static void h_insert(HTab *htab, HNode *node)
{
    size_t slot = node->hcode & htab->mask; // find the slot from the hash
    HNode *next = htab->tab[slot];          // current chain head at that slot
    node->next = next;                      // new node points to old head
    htab->tab[slot] = node;                 // new node becomes the head
    htab->size++;                           // one more key stored
}

// find a node by key. returns a pointer to the pointer that points to it,
// or NULL if not found. the caller supplies an equality function.
static HNode **h_lookup(HTab *htab, HNode *key, bool (*eq)(HNode *, HNode *))
{
    if (!htab->tab)
    {                // not initialized
        return NULL; // nothing to search
    }
    size_t slot = key->hcode & htab->mask; // which slot?
    HNode **from = &htab->tab[slot];       // pointer-to-pointer, starts at the slot

    for (HNode *cur; (cur = *from) != NULL; from = &cur->next)
    {
        if (cur->hcode == key->hcode && eq(cur, key))
        {                // hash matches AND key matches
            return from; // found — return pointer to it
        }
    }
    return NULL; // not found
}

// remove the node that `from` points to. returns the removed node.
static HNode *h_detach(HTab *htab, HNode **from)
{
    HNode *node = *from; // the node to remove
    *from = node->next;  // skip over it in the chain
    htab->size--;        // one fewer key
    return node;
}

// compare two nodes by their keys (used in h_lookup)
static bool entry_eq(HNode *lhs, HNode *rhs)
{
    Entry *le = container_of(lhs, Entry, node); // get the left half entry
    Entry *re = container_of(rhs, Entry, node); // get the right half entry

    return le->key == re->key; // compare key
}

// a resizable hashtable — uses two HTabs during rehashing
struct HMap
{
    HTab newer;             // new table (inserts go here)
    HTab older;             // old table (being drained during rehash)
    size_t migrate_pos = 0; // where we are in the migration of older
};

// begin rehashing: move newer → older, allocate a bigger newer
static void hm_trigger_rehashing(HMap *hmap)
{
    hmap->older = hmap->newer;                        // old new becomes "older"
    h_init(&hmap->newer, (hmap->newer.mask + 1) * 2); // allocate newer with double size
    hmap->migrate_pos = 0;                            // start migrating from slot 0
}

// migrate a few entries from older to newer. called on every operation.
static void hm_help_rehashing(HMap *hmap)
{
    if (hmap->older.tab == NULL)
    {
        return; // not rehashing
    }

    size_t nwork = 0;
    while (nwork < k_rehashing_work && hmap->older.size > 0)
    {
        // move to the next non-empty slot
        HNode **from = &hmap->older.tab[hmap->migrate_pos];
        if (!*from)
        {
            hmap->migrate_pos++; // empty slot — skip it
            continue;
        }

        // move one node from older to newer
        HNode *moved = h_detach(&hmap->older, from);
        h_insert(&hmap->newer, moved);
        nwork++;
    }

    // if older is empty, free it
    if (hmap->older.size == 0 && hmap->older.tab)
    {
        free(hmap->older.tab);
        hmap->older.tab = NULL;
    }
}

// find a node by key, checking both tables during rehashing
static HNode *hm_lookup(HMap *hmap, HNode *key, bool (*eq)(HNode *, HNode *))
{
    hm_help_rehashing(hmap); // do a little migration

    HNode **from = h_lookup(&hmap->newer, key, eq); // search newer first
    if (!from)
    {                                           // not in newer?
        from = h_lookup(&hmap->older, key, eq); // try older
    }
    return from ? *from : NULL; // dereference or return NULL
}

// insert a node. triggers rehashing when the table gets too full.
static void hm_insert(HMap *hmap, HNode *node)
{
    if (!hmap->newer.tab)
    {                            // first insert? initialize the table
        h_init(&hmap->newer, 4); // start with 4 slots
    }

    h_insert(&hmap->newer, node); // always insert into newer

    if (!hmap->older.tab)
    { // not already rehashing?
        size_t threshold = (hmap->newer.mask + 1) * k_max_load_factor;
        if (hmap->newer.size >= threshold)
        {
            hm_trigger_rehashing(hmap); // time to grow
        }
    }

    hm_help_rehashing(hmap); // do a little migration
}

// remove a node. searches both tables during rehashing.
static HNode *hm_delete(HMap *hmap, HNode *key, bool (*eq)(HNode *, HNode *))
{
    hm_help_rehashing(hmap); // do a little migration

    HNode **from = h_lookup(&hmap->newer, key, eq); // try newer
    if (from)
    {
        return h_detach(&hmap->newer, from); // found in newer — detach
    }

    from = h_lookup(&hmap->older, key, eq); // try older
    if (from)
    {
        return h_detach(&hmap->older, from); // found in older — detach
    }

    return NULL; // not found in either
}

// per Client state , remembered across event loop iterations
struct Conn
{
    int fd = -1;             // socket for this client
    bool want_read = false;  //"tell me when this client has data to read"
    bool want_write = false; //"tell me when this client has data to write"
    bool want_close = false; // "close this client at the end of the iteration"
                             // was: vector<uint8_t> incoming
    Buffer incoming;         // bytes received, not yet processed one byte per element
                             // was: vector<uint8_t> outgoing
    Buffer outgoing;         // bytes to send back /
};

// print error and exit
static void die(const char *msg)
{
    perror(msg);
    exit(1);
}

// make an fd non-blocking : read/write/accept/ return immediately
static void make_nonblocking(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0); // get current flags
    flags |= O_NONBLOCK;               // add the non-blocking flag
    fcntl(fd, F_SETFL, flags);         // set the flags
}

// try to read a 4-byte  uint_32 at 'cur'
// if enough bytes remain, store it in 'out' and advance 'cur' by 4.
//  returns false if not enough bytes
static bool read_u32(const uint8_t *&cur, const uint8_t *end, uint32_t &out)
{
    if (cur + 4 > end)
    {
        return false; // we do not have enough bytes
    }
    memcpy(&out, cur, 4); // copy 4 bytes into out
    cur += 4;             // advance cursor by 4
    return true;
}

// try to read `n` bytes at `cur` into `out`.
// if enough bytes remain, copy them and advance `cur` by n.
// returns false if not enough bytes.
static bool read_str(const uint8_t *&cur, const uint8_t *end, size_t n, string &out)
{
    if (cur + n > end)
    {
        return false; // not enough bytes
    }
    out.assign(cur, cur + n); // copy n bytes  into out
    cur += n;                 // move the cursor by n bytes
    return true;
}

// parse a request body into a list of strings.
// reutrns 0 on success, -1 on error
static int32_t parse_req(const uint8_t *data, size_t size, vector<string> &out)
{
    const uint8_t *end = data + size;
    const uint8_t *cur = data;

    // how many strings ?
    uint32_t nstr = 0;

    if (!read_u32(cur, end, nstr))
    {
        return -1;
    }
    if (nstr > 1000)
    { // safety limit
        return -1;
    }

    // read each string
    while (out.size() < nstr)
    {
        uint32_t len = 0;
        if (!read_u32(cur, end, len)) // read the length
        {
            return -1;
        }
        out.push_back(string()); // add empty string

        if (!read_str(cur, end, len, out.back())) // fill it out.back() points to last empty string
        {
            return -1;
        }
    }
    // make sure we consumed the whole message
    if (cur != end)
    {
        return -1; // trailing garbage
    }
    return 0;
}

// temporary key-value store — replaced with a real hashtable later
// static map<string, string> g_data;

// the global key-value store (our custom hashtable)
static HMap g_data;

// process a parsed command and fill the response
// static void do_request(vector<string> &cmd, Response &out)
// {
//     if (cmd.size() == 3 && cmd[0] == "set")
//     {
//         // set key value
//         g_data[cmd[1]].swap(cmd[2]); // we could used cmd[1] = cmd[2] , that will copy but we swap the pointers
//     }
//     else if (cmd.size() == 2 && cmd[0] == "get")
//     {
//         // get key
//         auto it = g_data.find(cmd[1]);
//         if (it == g_data.end())
//         {
//             out.status = RES_NX; // not found
//             return;
//         }
//         const string &val = it->second;
//         out.data.assign(val.begin(), val.end());
//     }
//     else if (cmd.size() == 2 && cmd[0] == "del")
//     {
//         // del key
//         size_t n = g_data.erase(cmd[1]);
//         out.data.assign((uint8_t *)&n, (uint8_t *)&n + 8); // 8-byte size_t
//     }
//     else
//     {
//         out.status = RES_ERR; // unknown command
//     }
// }
// write a nil response (just the tag)
static void out_nil(Buffer *out)
{
    buf_append_u8(out, TAG_NIL);
}

// write a string response: [TAG_STR][4-byte length][bytes]
static void out_str(Buffer *out, const char *s, size_t len)
{
    buf_append_u8(out, TAG_STR);
    buf_append_u32(out, (uint32_t)len);
    buf_append(out, (const uint8_t *)s, len);
}

// write an integer response: [TAG_INT][8-byte int64]
static void out_int(Buffer *out, int64_t val)
{
    buf_append_u8(out, TAG_INT);
    buf_append_i64(out, val);
}

// start writing an array. returns a position to patch later.
static size_t out_arr_begin(Buffer *out)
{
    buf_append_u8(out, TAG_ARR);     // tag
    size_t pos = buf_data_size(out); // remember where count goes
    buf_append_u32(out, 0);          // reserve 4 bytes for count
    return pos;                      // caller patches this later
}

// finish the array: write the actual count
static void out_arr_end(Buffer *out, size_t pos, uint32_t count)
{
    memcpy(buf_data(out) + pos, &count, 4); // patch the count
}

// write an error response: [TAG_ERR][4-byte code][4-byte len][message]
static void out_err(Buffer *out, uint32_t code, const char *msg)
{
    buf_append_u8(out, TAG_ERR);
    buf_append_u32(out, code);
    buf_append_u32(out, (uint32_t)strlen(msg));
    buf_append(out, (const uint8_t *)msg, strlen(msg));
}

// rewriteing the do_request functions with our custom hashTable
// process a parsed command and fill the response
static void do_request(vector<string> &cmd, Buffer *out)
{
    if (cmd.size() == 3 && cmd[0] == "set")
    {
        Entry key;
        key.key.swap(cmd[1]);
        key.node.hcode = str_hash((uint8_t *)key.key.data(), key.key.size());

        HNode *node = hm_lookup(&g_data, &key.node, &entry_eq);
        if (node)
        {
            // key exists — update value in place
            Entry *ent = container_of(node, Entry, node);
            ent->val.swap(cmd[2]);
        }
        else
        {
            // new key — create and insert
            Entry *ent = new Entry();
            ent->key.swap(key.key);
            ent->val.swap(cmd[2]);
            ent->node.hcode = key.node.hcode;
            hm_insert(&g_data, &ent->node);
        }
        out_nil(out); // set returns nil
    }
    else if (cmd.size() == 2 && cmd[0] == "get")
    {
        Entry key;
        key.key.swap(cmd[1]);
        key.node.hcode = str_hash((uint8_t *)key.key.data(), key.key.size());

        HNode *node = hm_lookup(&g_data, &key.node, &entry_eq);
        if (!node)
        {
            out_nil(out); // not found - nil
            return;
        }
        Entry *ent = container_of(node, Entry, node);
        out_str(out, ent->val.data(), ent->val.size()); // found a string
    }
    else if (cmd.size() == 2 && cmd[0] == "del")
    {
        Entry key;
        key.key.swap(cmd[1]);
        key.node.hcode = str_hash((uint8_t *)key.key.data(), key.key.size());

        HNode *node = hm_delete(&g_data, &key.node, &entry_eq);
        if (!node)
        {
            out_int(out, 0); // nothing deleted
            return;
        }
        Entry *ent = container_of(node, Entry, node);
        delete ent;
        out_int(out, 1); // one key deleted
    }
    else
    {
        out_err(out, ERR_UNKNOWN, "unkown command");
    }
}

// reserve 4bytes for the message header. remember where they are
static size_t response_begin(Buffer *out)
{
    size_t header_pros = buf_data_size(out); // position before writing
    uint8_t zero[4] = {0};                   // place holder
    buf_append(out, zero, 4);
    return header_pros;
}

// patch the reserved header with the actual message size
static void response_end(Buffer *out, size_t header_pros)
{
    size_t msg_size = buf_data_size(out) - header_pros - 4; // total minnus header
    uint32_t len = (uint32_t)msg_size;
    memcpy(buf_data(out) + header_pros, &len, 4);
}

// try to parse one request form conn->incoming
//  returns true if a request was parsed , false if we need more data
static bool try_one_request(Conn *conn)
{
    // need at least 4bytes for the length prefix
    if (buf_data_size(&conn->incoming) < 4)
    {
        return false; // not enough data yet
    }
    // read the 4 byte length prefix
    uint32_t len = 0;
    memcpy(&len, buf_data(&conn->incoming), 4);

    if (len > k_max_msg)
    {
        printf("message too long\n");
        conn->want_close = true;
        return false;
    }
    // need the full body too
    if (4 + len > buf_data_size(&conn->incoming))
    {
        return false; // body not fully arrived yet
    }

    // hardcoded response - request
    //  {   // we have a complete message
    //     const uint8_t *request = buf_data(&conn->incoming) + 4;
    //     // print only the first 32 chars to avoid spamming the terminal
    //     printf("client says %.32s%s (len=%u)\n", request, len > 32 ? "..." : "", len);
    //     // build the response : [4-byte length ][body]
    //     const char reply[] = "world";
    //     uint32_t reply_len = (uint32_t)strlen(reply);}

    // the message body is after 4-byte length prefix
    const uint8_t *body = buf_data(&conn->incoming) + 4;

    // parse the request into the list of strings
    vector<string> cmd;
    if (parse_req(body, len, cmd) < 0)
    {
        printf("bad request \n");
        conn->want_close = true;
        return false;
    }

    // reserve 4 bytes for the response
    size_t header_pros = response_begin(&conn->outgoing);

    // process the command - writes TLV directly to outgoing
    do_request(cmd, &conn->outgoing);

    // patch the response header with the actual size
    response_end(&conn->outgoing, header_pros);

    // consume the parsed request from incoming
    buf_consume(&conn->incoming, 4 + len);
    return true;
}

int main()
{
    // skeleton of our event loop

    // step1 socket()
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        die("socket()");

    printf("Got the Ticket from os fd = %d\n", fd);

    int val = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &val, sizeof(val));

    // step2: bind();
    struct sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(1234);
    addr.sin_addr.s_addr = htonl(0);

    int rv = bind(fd, (const struct sockaddr *)&addr, sizeof(addr));
    if (rv < 0)
        die("bind()");

    printf("Bound to port 1234!\n");

    // step3 listen
    rv = listen(fd, SOMAXCONN);
    if (rv < 0)
        die("listen()");

    printf("listening on port 1234!\n");

    // now make the listening fd non-blocking
    // so accept() never blocks; it returns -1 + EAGAIN if no client is listening

    make_nonblocking(fd);
    // map from fd to Conn pointer, indexed by fd
    // fd2conn[4] = the Conn for fd 4
    // fd2conn[7] = NULL means fd 7 has no Conn (unused)
    vector<Conn *> fd2conn; // which sockets ready

    // -- event loop --
    while (true)
    {
        // build the lists of fds for poll() to watch
        vector<struct pollfd> poll_args;

        // the first entry is always listening fd
        // we want to know when the new client connects
        struct pollfd pfd = {fd, POLLIN, 0};
        poll_args.push_back(pfd);

        // add every client's fd to the poll list
        for (Conn *conn : fd2conn)
        {
            if (!conn)
                continue; // empty slot

            struct pollfd cpfd = {conn->fd, POLLERR, 0};
            // pollerr always asks for error notifications

            if (conn->want_read)
            {
                cpfd.events |= POLLIN;
            }
            if (conn->want_write)
            {
                cpfd.events |= POLLOUT;
            }

            poll_args.push_back(cpfd);
        }

        // wait for readiness - this blocks until at least one fd is ready
        // -1 timeout blocksout forever until something is ready
        int nready = poll(poll_args.data(), (nfds_t)poll_args.size(), -1);
        if (nready < 0)
            die("poll()");

        // handle the listening fd-
        // poll_args[0] is the listening fd
        //  if revents has POLLIN, a new client is waiting to be accepted
        if (poll_args[0].revents & POLLIN)
        { // handling the listening fd
            struct sockaddr_in client_addr = {};
            socklen_t addrlen = sizeof(client_addr);
            int connfd = accept(fd, (struct sockaddr *)&client_addr, &addrlen);
            if (connfd < 0)
                continue;

            // make the new client fd non-blocking too--
            make_nonblocking(connfd);

            // create a Conn for this client
            Conn *conn = new Conn();
            conn->fd = connfd;
            conn->want_read = true; // we want to read the first request

            // store it in fd2Conn, indexed by fd
            // grow the array if needed
            if (fd2conn.size() <= (size_t)connfd)
            {
                fd2conn.resize(connfd + 1);
            }
            fd2conn[connfd] = conn; // it will point to the memory addres of client's conn state
            printf("New client connected ! connfd : %d\n", connfd);
        }

        // handle the client fds
        // poll_args[0] is the lisnening fd, so clients starts at index1
        for (size_t i = 1; i < poll_args.size(); i++)
        {
            uint32_t ready = poll_args[i].revents;
            Conn *conn = fd2conn[poll_args[i].fd];

            if (ready & POLLIN)
            {
                // read some bytes into temporary buffer
                uint8_t buf[64 * 1024];
                ssize_t rv = read(conn->fd, buf, sizeof(buf));

                if (rv <= 0)
                {
                    // client closed (rv ==0)or error(rv < 0);
                    printf("client %d is disconnected\n", conn->fd);
                    conn->want_close = true;
                    continue;
                }
                // append the bytes into the client's buffer
                buf_append(&conn->incoming, buf, rv);
                printf("client %d sent %zd bytes (total: %zu)\n", conn->fd, rv, buf_data_size(&conn->incoming));

                // try to parse as many complete requests as possible
                while (try_one_request(conn))
                {
                    // keep going while there are complete requests
                }
                // // if we produced a response , switch to want -write
                // if (conn->outgoing.size() > 0)
                // {
                //     conn->want_read = false;
                //     conn->want_write = true;
                // }

                // if we produced a response, try to write it now (optimistic)
                if (buf_data_size(&conn->outgoing) > 0)
                {
                    ssize_t rv = write(conn->fd, buf_data(&conn->outgoing), buf_data_size(&conn->outgoing));

                    if (rv < 0 && errno == EAGAIN)
                    {
                        // socket not ready right now - let poll() tell us later
                        conn->want_read = false;
                        conn->want_write = true;
                    }
                    else if (rv < 0)
                    {
                        // real error
                        printf("client %d write error\n", conn->fd);
                        conn->want_close = true;
                        continue;
                    }
                    else
                    {
                        // write succeeded - remove written bytes
                        buf_consume(&conn->outgoing, rv);
                        printf("client %d wrote %zd bytes (remaining : %zu)\n", conn->fd, rv, buf_data_size(&conn->outgoing));
                        // if everything went out , keep reading
                        if (buf_data_size(&conn->outgoing) == 0)
                        {
                            conn->want_read = true;
                            conn->want_write = false;
                        }
                        else
                        {
                            // partial write — need to wait for room
                            conn->want_read = false;
                            conn->want_write = true;
                        }
                    }
                }
            }
            if (ready & POLLOUT)
            {
                // write some bytes from outgoing to the socket
                ssize_t rv = write(conn->fd, buf_data(&conn->outgoing), buf_data_size(&conn->outgoing));

                if (rv <= 0)
                {
                    printf("client %d write error\n", conn->fd);
                    conn->want_close = true;
                    continue;
                }
                // remove written bytes from outgoing cause we have sent them to client no need to store taht
                buf_consume(&conn->outgoing, rv);

                printf("client %d wrote %zd bytes (remaining : %zu)\n", conn->fd, rv, buf_data_size(&conn->outgoing));

                // all sent go back to reading
                if (buf_data_size(&conn->outgoing) == 0)
                {
                    conn->want_read = true;
                    conn->want_write = false;
                }
            }
            if (ready & POLLERR)
            {
                printf("client %d has an error!\n", conn->fd);
            }
        }

        // cleanup clients marked up for close --
        for (size_t i = 1; i < poll_args.size(); i++)
        {
            Conn *conn = fd2conn[poll_args[i].fd]; // get the clients state
            if (!conn)
                continue; // if null continue
            if (conn->want_close)
            {
                close(conn->fd);
                fd2conn[conn->fd] = NULL;
                free_buffer(&conn->incoming); // free incoming buffer
                free_buffer(&conn->outgoing); // free outggoing buffer
                delete conn;
                printf("client cleaned up \n");
            }
        }
    }

    printf("setup done! \n");

    return 0;
}