#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "mocks.hpp"
#include "restore/block_serialization.hpp"
#include "restore/block_submission.hpp"
#include "restore/common.hpp"
#include "restore/core.hpp"

using namespace ::testing;

TEST(BlockSubmissionTest, exchangeData) {
    using BlockDistribution = ReStore::BlockDistribution<MPIContextMock>;
    using ReStoreMPI::RecvMessage;
    using ReStoreMPI::SendMessage;
    using SendBuffers = ReStore::BlockSubmissionCommunication<std::byte>::SendBuffers;

    std::vector<RecvMessage> dummyReceive;
    SendBuffers              emptySendBuffers;

    {
        MPIContextMock    mpiContext;
        BlockDistribution blockDistribution(10, 100, 3, mpiContext);

        ReStore::BlockSubmissionCommunication<uint8_t, MPIContextMock> comm(
            mpiContext, blockDistribution,
            ReStore::OffsetModeDescriptor{ReStore::OffsetMode::constant, sizeof(uint8_t)});

        std::vector<SendMessage> expectedSendMessages{{}};
        EXPECT_CALL(mpiContext, SparseAllToAll(Eq(expectedSendMessages))).WillOnce(Return(dummyReceive));
        ASSERT_NO_THROW(comm.exchangeData(emptySendBuffers));
    }

    {
        MPIContextMock    mpiContext;
        BlockDistribution blockDistribution(10, 100, 3, mpiContext);

        ReStore::BlockSubmissionCommunication<uint8_t, MPIContextMock> comm(
            mpiContext, blockDistribution,
            ReStore::OffsetModeDescriptor{ReStore::OffsetMode::constant, sizeof(uint8_t)});

        SendBuffers sendBuffers;
        sendBuffers[0] = {0_byte, 1_byte, 2_byte, 3_byte};
        sendBuffers[1] = {4_byte, 5_byte, 6_byte, 7_byte};

        std::vector<SendMessage> expectedSendMessages{{sendBuffers[0].data(), 4, 0}, {sendBuffers[1].data(), 4, 1}};

        EXPECT_CALL(mpiContext, SparseAllToAll(UnorderedElementsAreArray(expectedSendMessages)))
            .WillOnce(Return(dummyReceive));
        EXPECT_EQ(comm.exchangeData(sendBuffers), dummyReceive);
    }
}

TEST(BlockSubmissionTest, ParseIncomingMessages) {
    using BlockDistribution = ReStore::BlockDistribution<MPIContextMock>;
    using ReStore::OffsetMode;
    using ReStoreMPI::current_rank_t;
    using ReStoreMPI::RecvMessage;

    MPIContextMock    mpiContext;
    BlockDistribution blockDistribution(10, 100, 3, mpiContext);

    ReStore::BlockSubmissionCommunication<uint16_t, MPIContextMock> comm(
        mpiContext, blockDistribution, ReStore::OffsetModeDescriptor{OffsetMode::constant, sizeof(uint16_t)});

    RecvMessage message1(
        std::vector<std::byte>{
            // We have to write everything in big endian notation
            1_byte,    0_byte,    0_byte, 0_byte, 0_byte, 0_byte, 0_byte, 0_byte, // id 1 ...
            1_byte,    0_byte,    0_byte, 0_byte, 0_byte, 0_byte, 0_byte, 0_byte, // to 1
            0x02_byte, 0x02_byte,                                                 // id: 1, payload 0x0202
            3_byte,    0_byte,    0_byte, 0_byte, 0_byte, 0_byte, 0_byte, 0_byte, // id 3 ...
            3_byte,    0_byte,    0_byte, 0_byte, 0_byte, 0_byte, 0_byte, 0_byte, // to 3
            0x12_byte, 0x23_byte                                                  // id: 3, payload 0x3412
        },
        0);

    RecvMessage message2(
        std::vector<std::byte>{
            0_byte,    0_byte,    0_byte, 0_byte, 0_byte, 0_byte, 0_byte, 0_byte, // id 0 ...
            0_byte,    0_byte,    0_byte, 0_byte, 0_byte, 0_byte, 0_byte, 0_byte, // to 0
            0x37_byte, 0x13_byte,                                                 // payload 0x1337
            8_byte,    0_byte,    0_byte, 0_byte, 0_byte, 0_byte, 0_byte, 0_byte, // id 8 ...
            8_byte,    0_byte,    0_byte, 0_byte, 0_byte, 0_byte, 0_byte, 0_byte, // to 8
            0x42_byte, 0x00_byte,                                                 // payload 0x0042
            6_byte,    0_byte,    0_byte, 0_byte, 0_byte, 0_byte, 0_byte, 0_byte, // id 6 ...
            6_byte,    0_byte,    0_byte, 0_byte, 0_byte, 0_byte, 0_byte, 0_byte, // to 6
            0x11_byte, 0x11_byte                                                  // payload 0x1111
        },
        1);

    RecvMessage message3(
        std::vector<std::byte>{
            0_byte,    0_byte,    0_byte, 0_byte, 0_byte, 0_byte, 0_byte, 0_byte, // id 0 ...
            4_byte,    0_byte,    0_byte, 0_byte, 0_byte, 0_byte, 0_byte, 0_byte, // to 4
            0x02_byte, 0x00_byte,                                                 // payload 2
            0x03_byte, 0x00_byte,                                                 // payload 3
            0x04_byte, 0x00_byte,                                                 // payload 4
            0x05_byte, 0x00_byte,                                                 // payload 5
            0x06_byte, 0x00_byte                                                  // payload 6
        },
        2);


    auto called = 0;
    comm.parseIncomingMessage(
        message1,
        [&called](ReStore::block_id_t blockId, const std::byte* data, size_t lengthInBytes, current_rank_t srcRank) {
            if (called == 0) {
                ASSERT_EQ(blockId, 1);
                ASSERT_EQ(*reinterpret_cast<const uint16_t*>(data), 0x0202);
                ASSERT_EQ(lengthInBytes, 2);
                ASSERT_EQ(srcRank, 0);
            } else if (called == 1) {
                ASSERT_EQ(blockId, 3);
                ASSERT_EQ(*reinterpret_cast<const uint16_t*>(data), 0x2312);
                ASSERT_EQ(lengthInBytes, 2);
                ASSERT_EQ(srcRank, 0);
            } else {
                FAIL();
            }
            ++called;
        });
    ASSERT_EQ(called, 2);

    called = 0;
    comm.parseIncomingMessage(
        message2,
        [&called](ReStore::block_id_t blockId, const std::byte* data, size_t lengthInBytes, current_rank_t srcRank) {
            switch (called) {
                case 0:
                    ASSERT_EQ(blockId, 0);
                    ASSERT_EQ(*reinterpret_cast<const uint16_t*>(data), 0x1337);
                    ASSERT_EQ(lengthInBytes, 2);
                    ASSERT_EQ(srcRank, 1);
                    break;
                case 1:
                    ASSERT_EQ(blockId, 8);
                    ASSERT_EQ(*reinterpret_cast<const uint16_t*>(data), 0x0042);
                    ASSERT_EQ(lengthInBytes, 2);
                    ASSERT_EQ(srcRank, 1);
                    break;
                case 2:
                    ASSERT_EQ(blockId, 6);
                    ASSERT_EQ(*reinterpret_cast<const uint16_t*>(data), 0x1111);
                    ASSERT_EQ(lengthInBytes, 2);
                    ASSERT_EQ(srcRank, 1);
                    break;
                default:
                    FAIL();
            }
            ++called;
        });
    ASSERT_EQ(called, 3);

    called = 0;
    comm.parseIncomingMessage(
        message3,
        [&called](ReStore::block_id_t blockId, const std::byte* data, size_t lengthInBytes, current_rank_t srcRank) {
            ASSERT_EQ(blockId, called);
            ASSERT_EQ(*reinterpret_cast<const uint16_t*>(data), called + 2);
            ASSERT_EQ(lengthInBytes, 2);
            ASSERT_EQ(srcRank, 2);
            ++called;
        });
    ASSERT_EQ(called, 5);

    called = 0;
    std::vector<RecvMessage> messages{message1, message2, message3};
    comm.parseAllIncomingMessages(
        messages,
        [&called](ReStore::block_id_t blockId, const std::byte* data, size_t lengthInBytes, current_rank_t srcRank) {
            switch (called) {
                case 0:
                    ASSERT_EQ(blockId, 1);
                    ASSERT_EQ(*reinterpret_cast<const uint16_t*>(data), 0x0202);
                    ASSERT_EQ(lengthInBytes, 2);
                    ASSERT_EQ(srcRank, 0);
                    break;
                case 1:
                    ASSERT_EQ(blockId, 3);
                    ASSERT_EQ(*reinterpret_cast<const uint16_t*>(data), 0x2312);
                    ASSERT_EQ(lengthInBytes, 2);
                    ASSERT_EQ(srcRank, 0);
                    break;
                case 2:
                    ASSERT_EQ(blockId, 0);
                    ASSERT_EQ(*reinterpret_cast<const uint16_t*>(data), 0x1337);
                    ASSERT_EQ(lengthInBytes, 2);
                    ASSERT_EQ(srcRank, 1);
                    break;
                case 3:
                    ASSERT_EQ(blockId, 8);
                    ASSERT_EQ(*reinterpret_cast<const uint16_t*>(data), 0x0042);
                    ASSERT_EQ(lengthInBytes, 2);
                    ASSERT_EQ(srcRank, 1);
                    break;
                case 4:
                    ASSERT_EQ(blockId, 6);
                    ASSERT_EQ(*reinterpret_cast<const uint16_t*>(data), 0x1111);
                    ASSERT_EQ(lengthInBytes, 2);
                    ASSERT_EQ(srcRank, 1);
                    break;
                case 5:
                    ASSERT_EQ(blockId, 0);
                    ASSERT_EQ(*reinterpret_cast<const uint16_t*>(data), 2);
                    ASSERT_EQ(lengthInBytes, 2);
                    ASSERT_EQ(srcRank, 2);
                    break;
                case 6:
                    ASSERT_EQ(blockId, 1);
                    ASSERT_EQ(*reinterpret_cast<const uint16_t*>(data), 3);
                    ASSERT_EQ(lengthInBytes, 2);
                    ASSERT_EQ(srcRank, 2);
                    break;
                case 7:
                    ASSERT_EQ(blockId, 2);
                    ASSERT_EQ(*reinterpret_cast<const uint16_t*>(data), 4);
                    ASSERT_EQ(lengthInBytes, 2);
                    ASSERT_EQ(srcRank, 2);
                    break;
                case 8:
                    ASSERT_EQ(blockId, 3);
                    ASSERT_EQ(*reinterpret_cast<const uint16_t*>(data), 5);
                    ASSERT_EQ(lengthInBytes, 2);
                    ASSERT_EQ(srcRank, 2);
                    break;
                case 9:
                    ASSERT_EQ(blockId, 4);
                    ASSERT_EQ(*reinterpret_cast<const uint16_t*>(data), 6);
                    ASSERT_EQ(lengthInBytes, 2);
                    ASSERT_EQ(srcRank, 2);
                    break;
                default:
                    FAIL();
            }
            ++called;
        });
    ASSERT_EQ(called, 10);
}

TEST(BlockSubmissionTest, SerializeBlockForSubmission) {
    using BlockDistribution = ReStore::BlockDistribution<MPIContextMock>;
    using ReStore::block_id_t;
    using ReStore::NextBlock;
    using ReStore::OffsetMode;
    using ReStoreMPI::current_rank_t;
    using ReStoreMPI::original_rank_t;
    using ReStoreMPI::RecvMessage;

    struct World {
        bool    useMagic;
        uint8_t unicornCount;
    };

    NiceMock<MPIContextMock> mpiContext;
    EXPECT_CALL(mpiContext, getOnlyAlive(_)).WillRepeatedly([](std::vector<original_rank_t> ranks) {
        return getAliveOnlyFake({}, ranks);
    });

    auto blockDistribution = std::make_shared<BlockDistribution>(10, 100, 3, mpiContext);

    ReStore::BlockSubmissionCommunication<World, MPIContextMock> comm(
        mpiContext, *blockDistribution, ReStore::OffsetModeDescriptor{OffsetMode::constant, 2});

    World              earth       = {false, 0};
    World              narnia      = {true, 10};
    World              middleEarth = {true, 0};
    std::vector<World> worlds      = {earth, narnia, middleEarth};
    size_t             worldId     = 0;

    auto sendBuffers = comm.serializeBlocksForTransmission(
        [](World world, ReStore::SerializedBlockStoreStream& stream) {
            stream << world.unicornCount;
            stream << world.useMagic;
        },
        [&worlds, &worldId]() -> std::optional<NextBlock<World>> {
            auto ret = worldId < worlds.size() ? std::make_optional(NextBlock<World>({worldId, worlds[worldId]}))
                                               : std::nullopt;
            ++worldId;
            return ret;
        });

    // All these three blocks belong to range 0 and are therefore stored on ranks 0, 3 and 6
    // std::vector<std::byte> expectedSendBuffer = {
    //    0_byte, 0_byte, 0_byte, 0_byte, 0_byte, 0_byte, 0_byte, 0_byte, 0_byte,  0_byte, // earth
    //    1_byte, 0_byte, 0_byte, 0_byte, 0_byte, 0_byte, 0_byte, 0_byte, 10_byte, 1_byte, // narnia
    //    2_byte, 0_byte, 0_byte, 0_byte, 0_byte, 0_byte, 0_byte, 0_byte, 0_byte,  1_byte  // middle earth
    //};
    std::vector<std::byte> expectedSendBuffer = {
        0_byte,  0_byte, 0_byte, 0_byte, 0_byte, 0_byte, 0_byte, 0_byte, // from block id 0
        2_byte,  0_byte, 0_byte, 0_byte, 0_byte, 0_byte, 0_byte, 0_byte, // to block id 2
        0_byte,  0_byte,                                                 // earth
        10_byte, 1_byte,                                                 // narnia
        0_byte,  1_byte                                                  // middle earth
    };

    ASSERT_EQ(sendBuffers.size(), 3);

    ASSERT_EQ(sendBuffers[0].size(), 22);
    ASSERT_EQ(sendBuffers[3].size(), 22);
    ASSERT_EQ(sendBuffers[6].size(), 22);

    ASSERT_EQ(sendBuffers[0], expectedSendBuffer);
    ASSERT_EQ(sendBuffers[3], expectedSendBuffer);
    ASSERT_EQ(sendBuffers[6], expectedSendBuffer);
}
