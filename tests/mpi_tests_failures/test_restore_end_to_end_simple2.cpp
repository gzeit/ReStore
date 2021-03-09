#include <algorithm>
#include <cstddef>
#include <functional>
#include <signal.h>
#include <sstream>

#include "itertools.hpp"
#include <gmock/gmock.h>
#include <gtest-mpi-listener/include/gtest-mpi-listener.hpp>
#include <gtest/gtest.h>
#include <mpi.h>
#include <utility>

#include "restore/common.hpp"
#include "restore/core.hpp"
#include "restore/helpers.hpp"

#include "mocks.hpp"
#include "mpi_helpers.hpp"
#include "restore/mpi_context.hpp"

using namespace ::testing;

using iter::range;

TEST(ReStoreTest, EndToEnd_Simple2) {
    // TODO Assert the number of ranks (also in the other tests)
    // Each rank submits different data. The replication level is set to 3. There is no rank failure.
    ReStore::ReStore<int> store(MPI_COMM_WORLD, 3, ReStore::OffsetMode::constant, sizeof(int));

    std::vector<int> data;
    for (int value: range(1000 * myRankId(), 1000 * myRankId() + 1000)) {
        data.push_back(value);
    }

    unsigned counter = 0;
    store.submitBlocks(
        [](const int& value, ReStore::SerializedBlockStoreStream& stream) { stream << value; },
        [&counter, &data]() {
            auto ret = data.size() == counter
                           ? std::nullopt
                           : std::make_optional(ReStore::NextBlock<int>(
                               {counter + data.size() * static_cast<size_t>(myRankId()), data[counter]}));
            counter++; // We cannot put this in the above line, as we can't assume if the first argument of the pair is
                       // bound before or after the increment.
            return ret;
        },
        data.size() * static_cast<size_t>(numRanks()));

    // No failure
    std::vector<std::pair<std::pair<ReStore::block_id_t, size_t>, ReStoreMPI::current_rank_t>> requests;
    for (int rank = 0; rank < numRanks(); ++rank) {
        requests.emplace_back(
            std::make_pair(std::make_pair(static_cast<size_t>(rank) * data.size(), data.size()), rank));
    }

    std::vector<int>    dataReceived;
    ReStore::block_id_t nextBlockId = static_cast<size_t>(myRankId()) * data.size();
    store.pushBlocks(
        requests, [&dataReceived, &nextBlockId](const std::byte* dataPtr, size_t size, ReStore::block_id_t blockId) {
            EXPECT_EQ(nextBlockId, blockId);
            ++nextBlockId;
            ASSERT_EQ(sizeof(int), size);
            dataReceived.emplace_back(*reinterpret_cast<const int*>(dataPtr));
        });
    EXPECT_EQ(static_cast<size_t>(myRankId()) * data.size() + data.size(), nextBlockId);

    ASSERT_EQ(data.size(), dataReceived.size());
    for (size_t i = 0; i < data.size(); ++i) {
        EXPECT_EQ(data[i], dataReceived[i]);
    }
}

int main(int argc, char** argv) {
    // Filter out Google Test arguments
    ::testing::InitGoogleTest(&argc, argv);

    // Initialize MPI
    MPI_Init(&argc, &argv);
    // Set errorhandler to return so we have a chance to mitigate failures
    MPI_Comm_set_errhandler(MPI_COMM_WORLD, MPI_ERRORS_RETURN);

    // Add object that will finalize MPI on exit; Google Test owns this pointer
    ::testing::AddGlobalTestEnvironment(new GTestMPIListener::MPIEnvironment);

    // Get the event listener list.
    ::testing::TestEventListeners& listeners = ::testing::UnitTest::GetInstance()->listeners();

    // Remove default listener: the default printer and the default XML printer
    ::testing::TestEventListener* l = listeners.Release(listeners.default_result_printer());

    // Adds MPI listener; Google Test owns this pointer
    listeners.Append(new GTestMPIListener::MPIWrapperPrinter(l, MPI_COMM_WORLD));

    int result = RUN_ALL_TESTS();

    return result;
}
