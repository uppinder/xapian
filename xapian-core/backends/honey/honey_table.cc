
#include <config.h>

#include "honey_table.h"

#include "honey_cursor.h"

using Honey::RootInfo;

void
HoneyTable::create_and_open(int flags_, const RootInfo& root_info)
{
    flags = flags_;
    compress_min = root_info.get_compress_min();
    if (read_only) {
	num_entries = root_info.get_num_entries();
	root = root_info.get_root();
	// FIXME: levels
    }
    if (!fh.open(path, read_only))
	throw Xapian::DatabaseOpeningError("Failed to open HoneyTable", errno);
}

void
HoneyTable::open(int flags_, const RootInfo& root_info, honey_revision_number_t)
{
    flags = flags_;
    compress_min = root_info.get_compress_min();
    num_entries = root_info.get_num_entries();
    root = root_info.get_root();
    if (!fh.open(path, read_only)) {
	if (!lazy)
	    throw Xapian::DatabaseOpeningError("Failed to open HoneyTable", errno);
    }
}

void
HoneyTable::add(const std::string& key, const std::string& val, bool compressed)
{
    if (read_only)
	throw Xapian::InvalidOperationError("add() on read-only HoneyTable");
    if (key.size() == 0 || key.size() > 255)
	throw Xapian::InvalidArgumentError("Invalid key size: " + str(key.size()));
    if (key <= last_key)
	throw Xapian::InvalidOperationError("New key <= previous key");
    if (!last_key.empty()) {
	size_t len = std::min(last_key.size(), key.size());
	size_t i;
	for (i = 0; i < len; ++i) {
	    if (last_key[i] != key[i]) break;
	}
	fh.write(static_cast<unsigned char>(i));
	fh.write(static_cast<unsigned char>(key.size() - i));
	fh.write(key.data() + i, key.size() - i);
    } else {
	fh.write(static_cast<unsigned char>(key.size()));
	fh.write(key.data(), key.size());
    }
    ++num_entries;
    index.maybe_add_entry(key, fh.get_pos());

    // Encode "compressed?" flag in bottom bit.
    // FIXME: Don't do this if a table is uncompressed?  That saves a byte
    // for each item where the extra bit pushes the length up by a byte.
    size_t val_size_enc = (val.size() << 1) | compressed;
    std::string val_len;
    pack_uint(val_len, val_size_enc);
    // FIXME: pass together so we can potentially writev() both?
    fh.write(val_len.data(), val_len.size());
    fh.write(val.data(), val.size());
    last_key = key;
}

void
HoneyTable::commit(honey_revision_number_t, RootInfo* root_info)
{
    if (root < 0)
	throw Xapian::InvalidOperationError("root not set");

    root_info->set_level(1); // FIXME: number of index levels
    root_info->set_num_entries(num_entries);
    root_info->set_root_is_fake(false);
    // Not really meaningful.
    root_info->set_sequential(true);
    root_info->set_root(root);
    // Not really meaningful.
    root_info->set_blocksize(2048);
    // Not really meaningful.
    //root_info->set_free_list(std::string());

    read_only = true;
    fh.rewind();
    last_key = std::string();
}

bool
HoneyTable::read_item(std::string& key, std::string& val, bool& compressed) const
{
    if (!read_only) {
	return false;
    }

    int ch = fh.read();
    if (ch == EOF) return false;

    size_t reuse = 0;
    if (!last_key.empty()) {
	reuse = ch;
	ch = fh.read();
	if (ch == EOF) throw Xapian::DatabaseError("EOF/error while reading key length", errno);
    }
    size_t key_size = ch;
    char buf[4096];
    if (!fh.read(buf, key_size))
	throw Xapian::DatabaseError("read of " + str(key_size) + " bytes of key data failed", errno);
    key.assign(last_key, 0, reuse);
    key.append(buf, key_size);
    last_key = key;

    if (false) {
	std::string esc;
	description_append(esc, key);
	std::cout << "K:" << esc << std::endl;
    }

    int r;
    {
	// FIXME: rework to take advantage of buffering that's happening anyway?
	char * p = buf;
	for (int i = 0; i < 8; ++i) {
	    int ch2 = fh.read();
	    if (ch2 == EOF) {
		break;
	    }
	    *p++ = ch2;
	    if (ch2 < 128) break;
	}
	r = p - buf;
    }
    const char* p = buf;
    const char* end = p + r;
    size_t val_size;
    if (!unpack_uint(&p, end, &val_size)) {
	throw Xapian::DatabaseError("val_size unpack_uint invalid");
    }
    compressed = val_size & 1;
    val_size >>= 1;
    val.assign(p, end);
    if (p != end) std::abort();
    val_size -= (end - p);
    while (val_size) {
	size_t n = std::min(val_size, sizeof(buf));
	if (!fh.read(buf, n))
	    throw Xapian::DatabaseError("read of " + str(n) + "/" + str(val_size) + " bytes of value data failed", errno);
	val.append(buf, n);
	val_size -= n;
    }

    if (false) {
	std::string esc;
	description_append(esc, val);
	std::cout << "V:" << esc << std::endl;
    }

    return true;
}

bool
HoneyTable::get_exact_entry(const std::string& key, std::string& tag) const
{
    if (!read_only) std::abort();
    fh.rewind();
    last_key = std::string();
    std::string k, v;
    bool compressed;
    int cmp;
    do {
	if (!read_item(k, v, compressed)) return false;
	cmp = k.compare(key);
    } while (cmp < 0);
    if (cmp > 0) return false;
    if (compressed) {
	CompressionStream comp_stream;
	comp_stream.decompress_start();
	if (!comp_stream.decompress_chunk(v.data(), v.size(), tag)) {
	    // Decompression didn't complete.
	    abort();
	}
    } else {
	tag = v;
    }
    return true;
}

bool
HoneyTable::key_exists(const std::string& key) const
{
    if (!read_only) std::abort();
    fh.rewind();
    last_key = std::string();
    std::string k, v;
    bool compressed;
    int cmp;
    do {
	// FIXME: avoid reading tag data?
	if (!read_item(k, v, compressed)) return false;
	cmp = k.compare(key);
    } while (cmp < 0);
    return (cmp == 0);
}

HoneyCursor*
HoneyTable::cursor_get() const
{
    return new HoneyCursor(fh, root);
}