#include "t_metadata.h"
#include "t_hash.h"
#include "test_base.h"
#include <gtest/gtest.h>

TEST(InternalKey, EncodeAndDecode) {
  Slice key = "test-metadata-key";
  Slice sub_key = "test-metadata-sub-key";
  Slice ns = "namespace";
  uint64_t version = 12;
  std::string ns_key;
  PutFixed8(&ns_key, static_cast<uint8_t>(ns.size()));
  ns_key.append(ns.ToString());
  ns_key.append(key.ToString());
  InternalKey ikey(ns_key, sub_key, version);
  ASSERT_EQ(ikey.GetKey(), key);
  ASSERT_EQ(ikey.GetSubKey(), sub_key);
  ASSERT_EQ(ikey.GetVersion(), version);
  std::string bytes;
  ikey.Encode(&bytes);
  InternalKey ikey1(bytes);
  EXPECT_EQ(ikey, ikey1);
}

TEST(Metadata, EncodeAndDeocde) {
  std::string string_bytes;
  Metadata string_md(kRedisString);
  string_md.expire = 123;
  string_md.Encode(&string_bytes);
  Metadata string_md1(kRedisNone);
  string_md1.Decode(string_bytes);
  ASSERT_EQ(string_md, string_md1);
  ListMetadata list_md;
  list_md.flags = 13;
  list_md.expire = 123;
  list_md.version = 2;
  list_md.size = 1234;
  list_md.head = 123;
  list_md.tail = 321;
  ListMetadata list_md1;
  std::string list_bytes;
  list_md.Encode(&list_bytes);
  list_md1.Decode(list_bytes);
  ASSERT_EQ(list_md, list_md1);
}

class RedisTypeTest : public TestBase {
public:
  RedisTypeTest() :TestBase() {
    redis = new RedisDB(storage_, "default_ns");
    hash = new RedisHash(storage_, "default_ns");
    key_ = "test-redis-type";
    fields_ = {"test-hash-key-1", "test-hash-key-2", "test-hash-key-3"};
    values_  = {"hash-test-value-1", "hash-test-value-2", "hash-test-value-3"};
  }
  ~RedisTypeTest() {
    delete redis;
    delete hash;
  }
protected:
  RedisDB *redis;
  RedisHash *hash;
};

TEST_F(RedisTypeTest, GetMetadata) {
  int ret;
  std::vector<FieldValue> fvs;
  for (int i = 0; i < fields_.size(); i++) {
    fvs.emplace_back(FieldValue{fields_[i].ToString(), values_[i].ToString()});
  }
  rocksdb::Status s = hash->MSet(key_, fvs, false, &ret);
  EXPECT_TRUE(s.ok() && fvs.size()==ret);
  HashMetadata metadata;
  std::string ns_key;
  redis->AppendNamepacePrefix(key_, &ns_key);
  redis->GetMetadata(kRedisHash, ns_key, &metadata);
  EXPECT_EQ(fvs.size(), metadata.size);
  s = redis->Del(key_);
  EXPECT_TRUE(s.ok()) ;
}

TEST_F(RedisTypeTest, Expire) {
  int ret;
  std::vector<FieldValue> fvs;
  for (int i = 0; i < fields_.size(); i++) {
    fvs.emplace_back(FieldValue{fields_[i].ToString(), values_[i].ToString()});
  }
  rocksdb::Status s = hash->MSet(key_, fvs, false, &ret);
  EXPECT_TRUE(s.ok() && fvs.size()==ret);
  int64_t now;
  rocksdb::Env::Default()->GetCurrentTime(&now);
  redis->Expire(key_,int(now+2));
  int ttl;
  redis->TTL(key_, &ttl);
  ASSERT_TRUE(ttl >= 1 && ttl <= 2);
  sleep(2);
}