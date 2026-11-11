/*
	Untrunc - file.cpp

	Untrunc is GPL software; you can freely distribute,
	redistribute, modify & use under the terms of the GNU General
	Public License; either version 2 or its successor.

	Untrunc is distributed under the GPL "AS IS", without
	any warranty; without the implied warranty of merchantability
	or fitness for either an expressed or implied particular purpose.

	Please see the included GNU General Public License (GPL) for
	your rights and further details; see the file COPYING. If you
	cannot, write to the Free Software Foundation, 59 Temple Place
	Suite 330, Boston, MA 02111-1307, USA.  Or www.fsf.org

	Copyright 2010 Federico Ponchio

							*/

#include "file.h"
#include <string>
#include <cstring>
#include <iostream>

#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <libgen.h>

#include "common.h"
#include "upath.h"

#include <filesystem>

using namespace std;

std::vector<ZonaDanoSimulado> g_dano_simulado;
bool g_dano_simulado_activo = false;

void FileRead::danarLeido(uchar* dst, off_t pos, size_t n) {
	if (!danar_ || n == 0) return;
	const int64_t a = pos, b = pos + (int64_t)n;
	for (const auto& z : g_dano_simulado) {
		const int64_t i0 = std::max<int64_t>(a, z.ini), i1 = std::min<int64_t>(b, z.fin);
		if (i0 >= i1) continue;
		if (z.tipo == ZonaDanoSimulado::Ceros) {
			memset(dst + (i0 - a), 0, (size_t)(i1 - i0));
		} else if (z.tipo == ZonaDanoSimulado::Copia) {
			const off_t vuelta = ftello(file_);
			fseeko(file_, z.origen + (i0 - z.ini), SEEK_SET);
			size_t leidos = fread(dst + (i0 - a), 1, (size_t)(i1 - i0), file_);
			(void)leidos;
			fseeko(file_, vuelta, SEEK_SET);
		} else {
			// Pseudoaleatorio en funcion de la posicion: releer da lo mismo.
			for (int64_t p = i0; p < i1; p++) {
				uint64_t x = (uint64_t)p * 0x9E3779B97F4A7C15ull ^ ((uint64_t)z.semilla << 32);
				x ^= x >> 29; x *= 0xBF58476D1CE4E5B9ull; x ^= x >> 32;
				dst[p - a] = (uchar)x;
			}
		}
	}
}

FileRead::FileRead(const string& filename) {
	open(filename);
}

FileRead::~FileRead() {
	if(file_){
		fclose(file_);
		free(buffer_);
	}
}

void FileRead::open(const string& filename) {
	filename_ = filename;
	file_ = my_open(filename.c_str(), "rb");

	if (!file_) throw("Could not open file '" + filename + "': " + strerror(errno));

	fseeko(file_, 0L, SEEK_END);
	size_ = ftello(file_);
	fseeko(file_, 0L, SEEK_SET);

	if (!isRegularFile(fileno(file_))) throw("not a regular file: " + filename);

	buffer_ = (uchar*) malloc(buf_size_);
	danar_ = g_dano_simulado_activo && !g_dano_simulado.empty();
	size_t n = fread(buffer_, 1, buf_size_, file_);
	danarLeido(buffer_, 0, n);
}

void FileRead::seek(off_t p) {
	if (p < buf_begin_ || p >= buf_begin_ + buf_size_){
		fillBuffer(p);
	} else
		buf_off_ = p - buf_begin_;
}

void FileRead::seekSafe(off_t p) {
	seek(min(p, size_));
}

off_t FileRead::pos() {
	return buf_begin_ + buf_off_;
}

bool FileRead::atEnd() {
	return pos() >= size_;
}

size_t FileRead::fillBuffer(off_t location) {
	off_t avail = (buf_begin_+buf_size_) - location;
	off_t buf_loc = location - buf_begin_;

	buf_begin_ = location;
	buf_off_ = 0;
	if (avail < 0 || avail >= buf_size_) {
		fseeko(file_, location, SEEK_SET);
		int n = fread(buffer_, 1, buf_size_, file_);
		if (n > 0) danarLeido(buffer_, location, n);
		return n;
	}else if (avail > 0) {
		memmove(buffer_, buffer_+buf_loc, buf_size_-buf_loc);
	}
	const off_t donde = ftello(file_);
	int n = fread(buffer_+avail, 1, buf_size_-avail, file_);
	if (n > 0) danarLeido(buffer_ + avail, donde, n);
	return n;
}

size_t FileRead::readBuffer(uchar* dest, size_t size, size_t n) {
	logg(VV, "requests: ", size*n, " at offset : ", buf_off_, '\n');
	size_t total = size*n;
	size_t avail = buf_size_ - buf_off_;
	size_t nread = 0;
	if (avail < total) {
		logg(VV, "reallocating the file buffer\n");
		memcpy(dest, buffer_+buf_off_, avail);
		nread = avail;
		total -= avail;
		buf_off_ = buf_size_;
		if (total >= to_uint(buf_size_)){
			const off_t donde = ftello(file_);
			size_t x = fread(dest+nread, 1, total, file_);
			danarLeido(dest + nread, donde, x);
			nread += x;
			fillBuffer(ftello(file_));
		} else {
			size_t x = min(fillBuffer(buf_begin_+buf_off_), total);
			memcpy(dest+nread, buffer_, x);
			buf_off_ += x;
			nread += x;
		}
	} else {
		memcpy(dest, buffer_+buf_off_, total);
		buf_off_ += total;
		nread = total;
	}
	return nread/size;
}

uint FileRead::readInt() {
	int value;
	int n = readBuffer((uchar*)&value, sizeof(int), 1);
	if(n != 1) throw "Could not read integer";
	return swap32(value);
}

int64_t FileRead::readInt64() {
	int64_t value;
	int n = readBuffer((uchar*)&value, sizeof(value), 1);
	if(n != 1)
		throw "Could not read int64";

	return swap64(value);
}

string FileRead::getString(size_t n) {
	string r;
	r.resize(n);
	readChar(&r[0], n);
	return r;
}

void FileRead::readChar(char *dest, size_t n) {
	size_t len = readBuffer((uchar*)dest, sizeof(char), n);
	if(len != n){
		cout << "expected " << n << " but got " << len << '\n';
		throw "Could not read chars";
	}
}

vector<uchar> FileRead::read(size_t n) {
	vector<uchar> dest(n);
	size_t len = readBuffer(&*dest.begin(), 1, n);
	if(len != n)
		throw "Could not read at position";
	return dest;
}

const uchar* FileRead::getPtr(int size_requested) {
	// check if requested size exceeds buffer
	if (buf_off_ + size_requested > buf_size_){
		logg(VV, "size_requested: ", size_requested, '\n');
		fillBuffer(buf_begin_+buf_off_);
	}
	return buffer_+buf_off_;
}

const uchar* FileRead::getPtr2(int size_requested) {
	auto ret = getPtr(size_requested);
	buf_off_ += size_requested;
	return ret;
}

const uchar* FileRead::getPtrAt(off_t pos, int size_requested) {
	seek(pos);
	auto ret = getPtr(size_requested);
	buf_off_ += size_requested;
	return ret;
}

const uchar* FileRead::getFragment(off_t off, int size) {
	seek(off);
	return getPtr(size);
}

bool FileRead::isRegularFile(int fd) {
	struct stat s;
	fstat(fd, &s);
	return S_ISREG(s.st_mode);
}

bool FileRead::alreadyExists(const string& fn) {
	FILE *f;
	if ((f = my_open(fn.c_str(), "r"))) {
		fclose(f);
		return 1;
	}
	return 0;
}


FileWrite::FileWrite(const string& filename) {
	file_ = my_open(filename.c_str(), "wb");
	if(!file_)
		throw "Could not create file '" + filename + "': " + strerror(errno);
}

FileWrite::~FileWrite() {
	if(file_) fclose(file_);
}

off_t FileWrite::pos() {
	return ftello(file_);
}

int FileWrite::writeInt(int n) {
	n = swap32(n);
	fwrite(&n, sizeof(int), 1, file_);
	return 4;
}

int FileWrite::writeInt64(int64_t n) {
	n = swap64(n);
	fwrite(&n, sizeof(n), 1, file_);
	return 8;
}

int FileWrite::writeChar(const char *source, size_t n) {
	fwrite(source, 1, n, file_);
	return n;
}

int FileWrite::writeChar(const uchar *source, size_t n) {
	fwrite(source, 1, n, file_);
	return n;
}

int FileWrite::write(vector<uchar> &v) {
	fwrite(&*v.begin(), 1, v.size(), file_);
	return v.size();
}

void FileWrite::copyRange(FileRead& fin, size_t a, size_t b) {
	fin.seek(a);
	size_t n = b - a;
	size_t buff_sz = 1<<16;
	while (n) {
		cout << n << string(15, ' ') << '\r';
		auto to_read = min(buff_sz, n);
		auto p = fin.getPtr2(to_read);
		assert(to_read == fwrite(p, 1, to_read, file_));
		n -= to_read;
	}
}

void FileWrite::copyN(FileRead& fin, size_t start_off, size_t n) {
	copyRange(fin, start_off, start_off + n);
}

bool isdir(const string& path) {
	// stat() con char* no entiende UTF-8 en Windows: con una carpeta
	// "Mis Vídeos" la salida Reparados/ no se veia como carpeta y se
	// intentaba crear un ARCHIVO con ese nombre ("Permission denied").
	std::error_code ec;
	return std::filesystem::is_directory(upath::toPath(path), ec);
}

string myBasename(string path) {
	// basename may modifies its argument
	return basename(&path[0]);
}
