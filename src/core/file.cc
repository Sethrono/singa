#include <stdio.h>
#include <glob.h>
#include <glog/logging.h>
#include "core/file.h"
#include "core/common.h"
#include "google/protobuf/message.h"

/**
 * @file file.cc
 * Implementation of file related classes declared in file.h.
 */
namespace lapis {

static const int kFileBufferSize = 4 * 1024 * 1024; /**< file I/O buffer size */


vector<string> File::MatchingFilenames(StringPiece pattern) {
	glob_t globbuf;
	globbuf.gl_offs = 0;
	glob(pattern.AsString().c_str(), 0, NULL, &globbuf);
	vector < string > out;
	for (size_t i = 0; i < globbuf.gl_pathc; ++i) {
		out.push_back(globbuf.gl_pathv[i]);
	}
	globfree(&globbuf);
	return out;
}


void File::Mkdirs(string path) {
	if (path[0] != '/') {
		char cur[4096];
		getcwd(cur, 4096);
		path = string(cur) + "/" + path;
	}
	vector<StringPiece> pbits = StringPiece::split(path, "/");
	string prefix;
	for (size_t i = 0; i < pbits.size(); ++i) {
		pbits[i].strip();
		if (pbits[i].size() == 0) {
			continue;
		}
		prefix += "/" + pbits[i].AsString();
		int result = mkdir(prefix.c_str(), 0777);
		PCHECK(result == 0 || errno == EEXIST) << "Failed to create directory "
				<< path;
	}
}

/**
 * Slurp read a file by reading 32KB at a time.
 */
string File::Slurp(const string &f) {
  FILE *fp = fopen(f.c_str(), "r");
  CHECK(fp != NULL) << "Failed to read input file " << f;
  string out;
  char buffer[32768];
  while (!feof(fp) && !ferror(fp)) {
    int read = fread(buffer, 1, 32768, fp);
    if (read > 0) {
      out.append(buffer, read);
    } else {
      break;
    }
  }
  return out;
}

/**
 * Checking if a file exists by opening it in read mode.
 */
bool File::Exists(const string &f) {
	FILE *fp = fopen(f.c_str(), "r");
	if (fp) {
		fclose(fp);
		return true;
	}
	return false;
}

void File::Dump(const string &f, StringPiece data) {
	FILE *fp = fopen(f.c_str(), "w+");
	if (!fp) {
		LOG(FATAL) << "Failed to open output file " << f.c_str();
	}
	fwrite(data.data, 1, data.len, fp);
	fflush(fp);
	fclose(fp);
}

void File::Move(const string &src, const string &dst) {
	PCHECK(rename(src.c_str(), dst.c_str()) == 0);
}


RecordFile::RecordFile(const string &path, const string &mode,
		int compression) {
	path_ = path;
	mode_ = mode;
	if (mode == "r") {
		fp = new LocalFile(path_, mode);
	} else {
		fp = new LocalFile(path_ + ".tmp", mode);
	}
	if (compression == LZO) {
		fp = NULL; //new LZOFile((LocalFile*)fp, mode);
	}
}

RecordFile::~RecordFile() {
	if (!fp) {
		return;
	}
	if (mode_ != "r") {
		fp->sync();
		File::Move(StringPrintf("%s.tmp", path_.c_str()), path_);
	}
	delete fp;
}

/**
 * Write a Message object by serializing it to string and then call writeChunk().
 */
void RecordFile::write(const google::protobuf::Message &m) {
	writeChunk(m.SerializeAsString());
}

void RecordFile::writeChunk(StringPiece data) {
	int len = data.size();
	fp->write((char *) &len, sizeof(int));
	fp->write(data.data, data.size());
}

bool RecordFile::readChunk(string *s) {
	s->clear();
	int len;
	int bytes_read = fp->read((char *) &len, sizeof(len));
	if ((size_t) bytes_read < sizeof(int)) {
		return false;
	}

	s->resize(len);
	fp->read(&(*s)[0], len);
	return true;
}

bool RecordFile::read(google::protobuf::Message *m) {
	if (!readChunk(&buf_)) {
		return false;
	}
	if (!m)
		return true;
	CHECK(m->ParseFromString(buf_));
	return true;
}

void RecordFile::seek(uint64_t pos) {
	while (fp->tell() < pos && read(NULL))
		;
}

/**
 * Create a new log structured file for check-pointing.
 * When the file is first created (in "write" mode), writes the shard ID to
 * the begining of file.
 *
 * When the first is open for restored ("read" mode) or subsequent writing ("append" mode)
 * the read pointer simply moves to the end (SEEK_END).
 */
LogFile::LogFile(const string &path, const string &mode, int shard_id) {
	path_ = path;
	fp_ = fopen(path.c_str(), mode.c_str());
	if (!fp_) {
		char hostname[256];
		gethostname(hostname, sizeof(hostname));
	}
	setvbuf(fp_, NULL, _IONBF, kFileBufferSize);
	current_offset_ = 0;
	if (mode == "w") { //write header
		fwrite((char*) &shard_id, sizeof(int), 1, fp_);
	} else
		fseek(fp_, 0, SEEK_END); //move to the end
}

void LogFile::append(const string &key, const string &val, const int size) {
	int key_size = key.length();
	int val_size = val.length();
	int total_size = key_size + val_size + 2 * sizeof(int);
	int error = fwrite((char*) &key_size, sizeof(int), 1, fp_);
	fwrite(key.data(), key_size, 1, fp_);
	fwrite(val.data(), val_size, 1, fp_);
	fwrite((char*) &size, sizeof(int), 1, fp_);
	fwrite((char*) &total_size, sizeof(int), 1, fp_);

}

/**
 * Read the previous entry from the current file pointer.
 * 1. Move back 4-byte and read total record length (total_length)
 * 2. Move back total_length to read the entire record.
 * 3. Dis-assemble the records into key, value and the table size.
 */
void LogFile::previous_entry(Message *key, Message *val, int *size) {
	//read total length
	current_offset_ += sizeof(int);
	int total_length;
	fseek(fp_, -current_offset_, SEEK_END);
	fread((char*) &total_length, sizeof(int), 1, fp_);

	current_offset_ += total_length;
	fseek(fp_, -current_offset_, SEEK_END);

	string *buf = new string();
	buf->resize(total_length);
	fread(&(*buf)[0], total_length, 1, fp_);

	//read key
	int key_size;
	memcpy((char*) &key_size, &(*buf)[0], sizeof(int));
	key->ParseFromArray(&(*buf)[sizeof(int)], key_size);

	//read value
	int value_size = total_length - 2 * sizeof(int) - key_size;
	total_value_size += value_size;
	val->ParseFromArray(&(*buf)[sizeof(int) + key_size], value_size);

	//read current size
	memcpy((char*) size, &(*buf)[sizeof(int) + key_size + value_size],
			sizeof(int));
	delete buf;
}

/**
 * Move back to the file's beginning and read the shard ID.
 */
int LogFile::read_shard_id() {
	fseek(fp_, 0, SEEK_SET); // seek to the begining
	int shard_id;
	fread((char*) &shard_id, sizeof(int), 1, fp_);
	return shard_id;
}

/**
 * Read the latest record for the table size.
 */
int LogFile::read_latest_table_size() {
	fseek(fp_, -8, SEEK_END);
	int size;
	fread((char*) &size, sizeof(int), 1, fp_);
	fseek(fp_, 0, SEEK_END);
	return size;
}

} //namespace lapis
