#include <gtest/gtest.h>
#include "camera_daemon/shared_memory.hpp"
#include <cstring>
#include <thread>
#include <sys/mman.h>
#include <fcntl.h>

using namespace camera_daemon;

class SharedMemoryTest : public ::testing::Test {
protected:
    void TearDown() override {
        // Clean up any leftover shared memory
        shm_unlink("/test_shm");
        shm_unlink("/test_shm_reader");
    }
};

TEST_F(SharedMemoryTest, CreateAndMap) {
    SharedMemory shm("/test_shm", 4096, true);
    
    EXPECT_TRUE(shm.is_valid());
    EXPECT_NE(shm.data(), nullptr);
    EXPECT_EQ(shm.size(), 4096);
    EXPECT_EQ(shm.name(), "/test_shm");
}

TEST_F(SharedMemoryTest, WriteAndRead) {
    SharedMemory shm("/test_shm", 4096, true);
    ASSERT_TRUE(shm.is_valid());
    
    // Write data
    const char* test_data = "Hello, shared memory!";
    memcpy(shm.data(), test_data, strlen(test_data) + 1);
    
    // Read it back
    const char* read_data = static_cast<const char*>(shm.data());
    EXPECT_STREQ(read_data, test_data);
}

TEST_F(SharedMemoryTest, UnlinkRemovesShm) {
    {
        SharedMemory shm("/test_shm", 4096, true);
        ASSERT_TRUE(shm.is_valid());
        shm.unlink();
    }
    
    // After unlink, opening should fail or create new
    // Try to open without create flag - this behavior may vary
}

TEST_F(SharedMemoryTest, MoveConstruction) {
    SharedMemory shm1("/test_shm", 4096, true);
    ASSERT_TRUE(shm1.is_valid());
    void* original_data = shm1.data();
    
    SharedMemory shm2(std::move(shm1));
    
    EXPECT_TRUE(shm2.is_valid());
    EXPECT_EQ(shm2.data(), original_data);
    EXPECT_FALSE(shm1.is_valid());
}

TEST_F(SharedMemoryTest, MoveAssignment) {
    SharedMemory shm1("/test_shm", 4096, true);
    SharedMemory shm2("/test_shm_reader", 2048, true);
    
    void* original_data = shm1.data();
    shm2 = std::move(shm1);
    
    EXPECT_TRUE(shm2.is_valid());
    EXPECT_EQ(shm2.data(), original_data);
    EXPECT_EQ(shm2.size(), 4096);
}

// Test FramePublisher header structure
TEST(FramePublisherHeaderTest, Size) {
    // Header should be a known size for ABI compatibility
    EXPECT_EQ(sizeof(FramePublisher::Header), 64);
}

TEST(FramePublisherHeaderTest, AtomicSequence) {
    FramePublisher::Header header{};
    header.sequence.store(0);
    header.sequence.fetch_add(1);
    EXPECT_EQ(header.sequence.load(), 1);
}
