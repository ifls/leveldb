//
// Created by 贺逸峰 on 2020/7/12.
//

#include "db_test.h"


#include <iostream>
#include "db/log_writer.h"
#include "leveldb/env.h"
#include "util/crc32c.h"
#include "util/coding.h"

void writeRecord(leveldb::log::Writer *writer, int len, char flag) {
	leveldb::Status s;
	len++;
	char *str2 = (char *) malloc(sizeof(char) * (len));
	for (int i = 0; i < len; i++) {
		str2[i] = flag;
	};
	str2[len - 1] = 0x00;
	std::string data2 = str2;
	s = writer->AddRecord(data2);
	std::cout << s.ToString() << std::endl;
}

int main() {
	std::string file_name("log_writer.data");

	leveldb::WritableFile *file;
	leveldb::Status s = leveldb::Env::Default()->NewWritableFile(file_name, &file);

	leveldb::log::Writer writer(file);

	writeRecord(&writer, (1 << 15) - 13, '1');

	//如果结尾恰好7B空间，就会写一个0长的头部
	writeRecord(&writer, 15, '2');
	writeRecord(&writer, 1 << 20, '3');
	delete file;

	return 0;
}