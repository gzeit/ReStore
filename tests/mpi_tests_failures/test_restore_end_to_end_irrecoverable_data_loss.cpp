#include <algorithm>
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

TEST(ReStoreTest, EndToEnd_IrrecoverableDataLoss) {
    // Each rank submits different data. The replication level is set to 2. There are three rank failures. Therefore,
    // some data should be irrecoverably lost.
    ReStore::ReStore<int> store(MPI_COMM_WORLD, 2, ReStore::OffsetMode::constant, sizeof(int));

    std::vector<int> data;
    for (int value: range(1000 * myRankId(), 1000 * myRankId() + 1000)) {
        data.push_back(value);
    }

    auto numBlocks = data.size() * asserting_cast<size_t>(numRanks());

    unsigned counter = 0;
    store.submitBlocks(
        [](const int& value, ReStore::SerializedBlockStoreStream& stream) { stream << value; },
        [&counter, &data]() {
            auto ret = data.size() == counter
                           ? std::nullopt
                           : std::make_optional(ReStore::NextBlock<int>(
                               {counter + asserting_cast<size_t>(myRankId()) * 1000, data[counter]}));
            counter++; // We cannot put this in the above line, as we can't assume if the first argument of the pair is
                       // bound before or after the increment.
            return ret;
        },
        numBlocks);

    // Two failures
    constexpr int failingRank1 = 1;
    // constexpr int failingRank2 = 2;
    constexpr int failingRank3 = 3;
    failRank(failingRank1);
    // failRank(failingRank2);
    failRank(failingRank3);
    ASSERT_NE(myRankId(), failingRank1);
    // ASSERT_NE(myRankId(), failingRank2);
    ASSERT_NE(myRankId(), failingRank3);

    auto newComm = getFixedCommunicator();

    store.updateComm(newComm);

    std::vector<std::pair<std::pair<ReStore::block_id_t, size_t>, ReStoreMPI::current_rank_t>> requests;
    for (const auto rank: range(numRanks(newComm))) {
        requests.emplace_back(std::make_pair(std::make_pair(0, numBlocks), rank));
    }

    EXPECT_THROW(
        store.pushBlocks(requests, [](const std::byte*, size_t, ReStore::block_id_t) {}),
        ReStore::UnrecoverableDataLossException);
}

int main(int argc, char** argv) {
    // Filter out Google Test arguments
    ::testing::InitGoogleTest(&argc, argv);

    // Initialize MPI
    MPI_Init(&argc, &argv);
    // Set errorhandler to return so we have a chance to mitigate failures
    MPI_Comm_set_errhandler(MPI_COMM_WORLD, MPI_ERRORS_RETURN);

    // // Add object that will finalize MPI on exit; Google Test owns this pointer
    // ::testing::AddGlobalTestEnvironment(new GTestMPIListener::MPIEnvironment);

    // // Get the event listener list.
    // ::testing::TestEventListeners& listeners = ::testing::UnitTest::GetInstance()->listeners();

    // // Remove default listener: the default printer and the default XML printer
    // ::testing::TestEventListener* l = listeners.Release(listeners.default_result_printer());

    // // Adds MPI listener; Google Test owns this pointer
    // listeners.Append(new GTestMPIListener::MPIWrapperPrinter(l, MPI_COMM_WORLD));

    int result = RUN_ALL_TESTS();

    return result;
}
