#ifndef RESTORE_CORE_H
#define RESTORE_CORE_H

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <vector>

//#include "backward.hpp"
#include "mpi_context.hpp"
#include <mpi.h>

#include "helpers.hpp"
#include "restore/block_distribution.hpp"
#include "restore/block_serialization.hpp"
#include "restore/block_submission.hpp"
#include "restore/common.hpp"

namespace ReStore {

class UnrecoverableDataLossException : public std::exception {
    virtual const char* what() const throw() override {
        return "Unrecoverable data loss occurred.";
    }
};

template <class BlockType>
class ReStore {
    using Communication = BlockSubmissionCommunication<BlockType>;

    public:
    // Constructor
    //
    // mpiCommunicator: The MPI Communicator to use.
    // replicationLevel: The number of replicas to distribute among the different ranks.
    // offsetMode: When the serialized blocks are stored in memory, we need to know at which memory location
    //      each block starts. For large blocks, we can afford a look-up-table. This has the advantage that
    //      we can handle blocks with different lengths. For very small blocks, however, a look-up-table would
    //      incur too much of an memory overhead. Take for example block sizes of 4 bytes, resulting in millions
    //      or billions of blocks per rank. By using a constant offset, we can still store that many blocks without
    //      a look-up-table. The drawback is, that each block will take up constOffset _bytes_ of space.
    // constOffset: An upper bound for the number of _bytes_ a serialized block takes up. Has to be equal to 0
    //      when using look-up-table offset mode and greater than 0 when using consOffset mode.
    ReStore(MPI_Comm mpiCommunicator, uint16_t replicationLevel, OffsetMode offsetMode, size_t constOffset = 0)
        : _replicationLevel(replicationLevel),
          _offsetMode(offsetMode),
          _constOffset(constOffset),
          _mpiContext(mpiCommunicator),
          _blockDistribution(nullptr), // Depends on the number of blocks which are submitted in submitBlocks.
          _serializedBlocks(nullptr) { // Depends on _blockDistribution
        if (offsetMode == OffsetMode::lookUpTable && constOffset != 0) {
            throw std::runtime_error("Explicit offset mode set but the constant offset is not zero.");
        } else if (offsetMode == OffsetMode::constant && constOffset == 0) {
            throw std::runtime_error("Constant offset mode required a constOffset > 0.");
        } else if (replicationLevel == 0) {
            throw std::runtime_error("What is a replication level of 0 supposed to mean?");
        } else {
            _assertInvariants();
        }
    }

    // Copying a ReStore object does not really make sense. It would be really hard and probably not
    // what you want to deep copy the replicated blocks (including the remote ones?), too.
    ReStore(const ReStore& other) = delete;
    ReStore& operator=(const ReStore& other) = delete;

    // Moving a ReStore is disabled for now, because we do not need it and use const members
    ReStore(ReStore&& other) = delete;
    ReStore& operator=(ReStore&& other) = delete;

    // Destructor
    ~ReStore() = default;

    // replicationLevel()
    //
    // Get the replication level, that is how many copies of each block are scattered over the ranks.
    uint32_t replicationLevel() const noexcept {
        _assertInvariants();
        return this->_replicationLevel;
    }

    // offsetMode()
    //
    // Get the offset mode that defines how the serialized blocks are aligned in memory.
    std::pair<OffsetMode, size_t> offsetMode() const noexcept {
        _assertInvariants();
        return std::make_pair(this->_offsetMode, this->_constOffset);
    }

    // submitBlocks()
    //
    // Submits blocks to the replicated storage. They will be replicated among the ranks and can be
    // ReStored after a rank failure. Each rank has to call this function exactly once.
    // submitBlocks() also performs the replication and is therefore blocking until all ranks called it.
    // Even if there are multiple receivers for a single block, serialize will be called only once per block.
    //
    // serializeFunc: gets a reference to a block to serialize and a stream which can be used
    //      to append a flat representation of the current block to the serialized data's byte stream.
    // nextBlock: a generator function which should return <globalBlockId, const reference to block>
    //      on each call. If there are no more blocks getNextBlock should return {}.
    // totalNumberOfBlocks: The total number of blocks across all ranks.
    // canBeParallelized: Indicates if multiple serializeFunc calls can happen on different blocks
    //      concurrently. Also assumes that the blocks do not have to be serialized in the order they
    //      are emitted by nextBlock.
    // If a rank failure happens during this call, it will be propagated to the caller which can then handle it. This
    // includes updating the communicator of MPIContext.
    template <class SerializeBlockCallbackFunction, class NextBlockCallbackFunction>
    void submitBlocks(
        SerializeBlockCallbackFunction serializeFunc, NextBlockCallbackFunction nextBlock, size_t totalNumberOfBlocks,
        bool canBeParallelized = false // not supported yet
    ) {
        static_assert(
            std::is_invocable<SerializeBlockCallbackFunction, const BlockType&, SerializedBlockStoreStream>(),
            "serializeFunc must be invocable as _(const BlockType&, SerializedBlockStoreStream");
        static_assert(
            std::is_invocable_r<std::optional<NextBlock<BlockType>>, NextBlockCallbackFunction>(),
            "serializeFunc must be invocable as ReStore::std::optional<NextBlock<BlockType>>()");

        if (totalNumberOfBlocks == 0) {
            throw std::runtime_error("Invalid number of blocks: 0.");
        }

        try { // Ranks failures might be detected during this block
            // We define original rank ids to be the rank ids during this function call
            _mpiContext.resetOriginalCommToCurrentComm();

            // Initialize the Block Distribution
            if (_blockDistribution) {
                throw std::runtime_error("You shall not call submitBlocks() twice!");
            }
            _blockDistribution = std::make_shared<BlockDistribution<>>(
                _mpiContext.getOriginalSize(), totalNumberOfBlocks, _replicationLevel, _mpiContext);
            assert(_serializedBlocks);
            _serializedBlocks =
                std::make_unique<SerializedBlockStorage<>>(_blockDistribution, _offsetMode, _constOffset);
            assert(_mpiContext.getOriginalSize() == _mpiContext.getCurrentSize());

            // Initialize the Implementation object (as in PImpl)
            BlockSubmissionCommunication<BlockType> comm(_mpiContext, *_blockDistribution);

            // Allocate send buffers and serialize the blocks to be sent
            auto sendBuffers = comm.serializeBlocksForTransmission(serializeFunc, nextBlock, canBeParallelized);

            // All blocks have been serialized, send & receive replicas
            auto receivedMessages = comm.exchangeData(sendBuffers);

            // Store the received blocks into our local block storage
            comm.parseAllIncomingMessages(
                receivedMessages,
                [this](
                    block_id_t blockId, const uint8_t* data, size_t lengthInBytes, ReStoreMPI::current_rank_t srcRank) {
                    UNUSED(lengthInBytes); // Currently, only constant offset mode is implemented
                    UNUSED(srcRank);       // We simply do not need this right now
                    this->_serializedBlocks->writeBlock(blockId, data);
                },
                offsetMode());
        } catch (ReStoreMPI::FaultException& e) {
            // Reset BlockDistribution and SerializedBlockStorage
            _blockDistribution = nullptr;
            _serializedBlocks  = nullptr;
            throw e;
        }
    }

    // pullBlocks()
    //
    // Pulls blocks from other ranks in the replicated storage. That is, the caller provides the global // ids of those
    // blocks it wants but not from which rank to fetch them. This means that we have to perform an extra round of
    // communication compared with pushBlocks() to // request the blocks each rank wants.
    //
    // blockRanges: A list of ranges of global blck ids <firstId, numberOfBlocks> this rank wants
    // handleSerializedBlock: A function which takes a void * pointing to the start of the serialized
    //      byte stream, a length in bytes of this encoding and the global id of this block.
    // canBeParallelized: Indicates if multiple handleSerializedBlock calls can happen on different
    //      inputs concurrently.
    template <class HandleSerializedBlockFunction>
    void pullBlocks(
        std::vector<std::pair<block_id_t, size_t>> blockRanges, HandleSerializedBlockFunction handleSerializedBlock,
        bool canBeParallelized = false // not supported yet
    ) {
        // TODO implement
        assert(false);
        UNUSED(blockRanges);
        UNUSED(handleSerializedBlock);
        UNUSED(canBeParallelized);
        // HandleSerializedBlockFunction void(SerializedBlockOutStream, size_t lengthOfStreamInBytes, block_id_t)
    }

    using block_range_external_t = std::pair<block_id_t, size_t>;
    using block_range_request_t  = std::pair<block_range_external_t, int>;

    // pushBlocks()
    //
    // Pushes blocks to other ranks in the replicated storage. That is, the caller provides the global
    // ids of those blocks it has to sent and where to send it to. For the receiver to know which of its
    // received blocks corresponds to which global id, the same information has to be provided on the
    // receiver side.
    // This function is for example useful if each rank knows the full result of the load balancer. In
    // this scenario, each rank knows which blocks each other rank needs. Compared to pullBlocks() we
    // therefore don't need to communicate the requests for block ranges over the network.
    //
    // blockRanges: A list of <blockRange, destinationRank> where a block range is a tuple of global
    //      block ids <firstId, numberOfBlocks>
    // handleSerializedBlock: A function which takes a void * pointing to the start of the serialized
    //      byte stream, a length in bytes of this encoding and the global id of this block.
    // canBeParallelized: Indicates if multiple handleSerializedBlock calls can happen on different
    //      inputs concurrently.
    template <class HandleSerializedBlockFunction>
    void pushBlocks(
        const std::vector<std::pair<std::pair<block_id_t, size_t>, int>>& blockRanges,
        HandleSerializedBlockFunction                                     handleSerializedBlock,
        bool                                                              canBeParallelized = false // not supported yet
    ) {
        UNUSED(canBeParallelized);
        const auto [sendBlockRanges, recvBlockRanges] = getSendRecvBlockRanges(blockRanges);
        const auto recvMessages                       = sparseAllToAll(sendBlockRanges);
        handleReceivedBlocks(recvMessages, recvBlockRanges, handleSerializedBlock);
    }

    private:
    const uint16_t                            _replicationLevel;
    const OffsetMode                          _offsetMode;
    const size_t                              _constOffset;
    ReStoreMPI::MPIContext                    _mpiContext;
    std::shared_ptr<BlockDistribution<>>      _blockDistribution;
    std::unique_ptr<SerializedBlockStorage<>> _serializedBlocks;


    template <class HandleSerializedBlockFunction>
    void handleReceivedBlocks(
        const std::vector<ReStoreMPI::RecvMessage>& recvMessages,
        const std::vector<block_range_request_t>&   recvBlockRanges,
        HandleSerializedBlockFunction               handleSerializedBlock) {
        static_assert(
            std::is_invocable<HandleSerializedBlockFunction, const void*, size_t, block_id_t>(),
            "HandleSerializedBlockFunction must be invocable as (const uint8_t*, size_t, "
            "block_id_t)");
        size_t currentIndexRecvBlockRanges = 0;
        for (const ReStoreMPI::RecvMessage& recvMessage: recvMessages) {
            assert(currentIndexRecvBlockRanges < recvBlockRanges.size());
            assert(recvMessage.srcRank == recvBlockRanges[currentIndexRecvBlockRanges].second);
            // TODO: Implement LUT mode
            assert(_offsetMode == OffsetMode::constant);
            size_t currentIndexRecvMessage = 0;
            while (currentIndexRecvBlockRanges < recvBlockRanges.size()
                   && recvBlockRanges[currentIndexRecvBlockRanges].second == recvMessage.srcRank) {
                for (block_id_t blockId = recvBlockRanges[currentIndexRecvBlockRanges].first.first;
                     blockId < recvBlockRanges[currentIndexRecvBlockRanges].first.first
                                   + recvBlockRanges[currentIndexRecvBlockRanges].first.second;
                     ++blockId) {
                    assert(currentIndexRecvMessage < recvMessage.data.size());
                    assert(currentIndexRecvMessage + +_constOffset < recvMessage.data.size());
                    handleSerializedBlock(&(recvMessage.data[currentIndexRecvMessage]), _constOffset, blockId);
                    currentIndexRecvMessage += _constOffset;
                }
                currentIndexRecvBlockRanges++;
            }
        }
    }

    std::vector<ReStoreMPI::RecvMessage> sparseAllToAll(const std::vector<block_range_request_t>& sendBlockRanges) {
        std::vector<std::vector<uint8_t>>    sendData;
        std::vector<ReStoreMPI::SendMessage> sendMessages;
        int                                  currentRank = MPI_UNDEFINED;
        for (const block_range_request_t& sendBlockRange: sendBlockRanges) {
            if (currentRank != sendBlockRange.second) {
                assert(currentRank < sendBlockRange.second);
                if (currentRank != MPI_UNDEFINED) {
                    assert(sendData.size() > 0);
                    sendMessages.emplace_back(sendData.back().data(), sendData.back().size(), currentRank);
                }
                sendData.emplace_back();
                currentRank = sendBlockRange.second;
            }
            // TODO Implement LUT mode
            assert(_offsetMode == OffsetMode::constant);
            _serializedBlocks->forAllBlocks(sendBlockRange.first, [&sendData](uint8_t* ptr, size_t size) {
                sendData.back().insert(sendData.back().end(), ptr, ptr + size);
            });
        }
        assert(currentRank != MPI_UNDEFINED);
        assert(sendData.size() > 0);
        sendMessages.emplace_back(sendData.back().data(), sendData.back().size(), currentRank);
        auto result = _mpiContext.SparseAllToAll(sendMessages);
        std::sort(
            result.begin(), result.end(), [](const ReStoreMPI::RecvMessage& lhs, const ReStoreMPI::RecvMessage& rhs) {
                return lhs.srcRank < rhs.srcRank;
            });
        return result;
    }

    std::pair<std::vector<block_range_request_t>, std::vector<block_range_request_t>>
    getSendRecvBlockRanges(const std::vector<block_range_request_t>& blockRanges) {
        std::vector<block_range_request_t> sendBlockRanges;
        std::vector<block_range_request_t> recvBlockRanges;
        for (const auto& blockRange: blockRanges) {
            for (block_id_t blockId = blockRange.first.first;
                 blockId < blockRange.first.first + blockRange.first.second;
                 blockId += _blockDistribution->rangeOfBlock(blockId).length()) {
                const auto                        blockRangeInternal = _blockDistribution->rangeOfBlock(blockId);
                const ReStoreMPI::original_rank_t servingRank        = getServingRank(blockRangeInternal);
                size_t                            size               = blockRangeInternal.length();
                if (blockRangeInternal.start() + blockRangeInternal.length()
                    >= blockRange.first.first + blockRange.first.second) {
                    size = blockRange.first.first + blockRange.first.second - blockRangeInternal.start();
                }
                if (servingRank == _mpiContext.getMyOriginalRank()) {
                    sendBlockRanges.emplace_back(std::make_pair(blockRangeInternal.start(), size), blockRange.second);
                }
                if (blockRange.second == _mpiContext.getMyCurrentRank()) {
                    recvBlockRanges.emplace_back(
                        std::make_pair(blockRangeInternal.start(), size), _mpiContext.getCurrentRank(servingRank));
                }
            }
        }
        auto sortByRankAndBegin = [](const block_range_request_t& lhs, const block_range_request_t& rhs) {
            bool ranksLess   = lhs.second < rhs.second;
            bool ranksEqual  = lhs.second == rhs.second;
            bool blockIdLess = lhs.first.first < rhs.first.first;
            return ranksLess || (ranksEqual && blockIdLess);
        };
        std::sort(sendBlockRanges.begin(), sendBlockRanges.end(), sortByRankAndBegin);
        std::sort(recvBlockRanges.begin(), recvBlockRanges.end(), sortByRankAndBegin);
        return std::make_pair(sendBlockRanges, recvBlockRanges);
    }

    void _assertInvariants() const {
        assert(
            (_offsetMode == OffsetMode::constant && _constOffset > 0)
            || (_offsetMode == OffsetMode::lookUpTable && _constOffset == 0));
        assert(_replicationLevel > 0);
    }

    int getServingRank(const BlockDistribution<>::BlockRange& blockRange) {
        auto ranksWithBlockRange = _blockDistribution->ranksBlockRangeIsStoredOn(blockRange);
        if (ranksWithBlockRange.empty()) {
            throw UnrecoverableDataLossException();
        }
        // TODO: Is this smart? Maybe even split up blocks
        return ranksWithBlockRange.front();
    }
}; // class ReStore

/*
Indended usage:

Storage storage;
// storage.setProcessMap(...) --- Skipped for now
storage.setReplication(k)
storage.setOffsetMode(constant|explicit, size_t c = 0)
storage.submitBlocks(...)

!! failure !!
pushPullBlocks(...) || pullBlocks(...)
*/

} // namespace ReStore
#endif // RESTORE_CORE_H
