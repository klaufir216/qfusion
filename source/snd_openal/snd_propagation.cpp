#include "snd_propagation.h"
#include "snd_computation_host.h"
#include "snd_allocators.h"

#include "../gameshared/q_collision.h"
#ifdef min
#undef min
#endif
#ifdef max
#undef max
#endif

#include "../qalgo/Links.h"
#include "../qalgo/SingletonHolder.h"

#include <algorithm>
#include <limits>
#include <memory>
#include <random>

template <typename AdjacencyListType, typename DistanceType>
GraphLike<AdjacencyListType, DistanceType>::~GraphLike() {
	TaggedAllocators::FreeUsingMetadata( distanceTable, "distance table" );
	TaggedAllocators::FreeUsingMetadata( adjacencyListsData, "adjacency data" );
}

template <typename AdjacencyListType, typename DistanceType>
class MutableGraph: public GraphLike<AdjacencyListType, DistanceType> {
	template <typename, typename> friend class GraphBuilder;
protected:
	explicit MutableGraph( int numLeafs_ )
		: GraphLike<AdjacencyListType, DistanceType>( numLeafs_ ) {}

	/**
	 * An address of the buffer that is allowed to be modified.
	 * The {@code distanceTable} must point to it after {@code SaveDistanceTable()} call.
	 * @note It is intended to be different for different instances of a mutable graph if there are multiple ones.
	 */
	DistanceType *distanceTableScratchpad { nullptr };
	/**
	 * An address of the original buffer that must be kept unmodified.
	 * The {@code distanceTable} must point to it initially and after {@code RestoreDistanceTable} call.
	 * @note It is intended to be shared between instances of a mutable graph if there are multiple ones.
	 */
	DistanceType *distanceTableBackup { nullptr };
public:
	~MutableGraph() override {
		TaggedAllocators::FreeUsingMetadata( distanceTableBackup, "distance table backup" );
		TaggedAllocators::FreeUsingMetadata( distanceTableScratchpad, "distance table scratchpad" );
		// Prevent double-free in the parent constructor
		this->distanceTable = nullptr;
	}

	void SetEdgeDistance( int leaf1, int leaf2, DistanceType newDistance ) {
		// Template quirks: a member of a template base cannot be resolved in scope otherwise
		auto *const distanceTable = this->distanceTable;
		// Just check whether the distance table is set.
		// Unfortunately this method can be used outside SaveDistanceTable()/RestoreDistanceTable() context
		assert( distanceTable );
		// The distance table must point at the scratchpad
		const int numLeafs = this->numLeafs;
		assert( leaf1 > 0 && leaf1 < numLeafs );
		assert( leaf2 > 0 && leaf2 < numLeafs );
		distanceTable[leaf1 * numLeafs + leaf2] = newDistance;
		distanceTable[leaf2 * numLeafs + leaf1] = newDistance;
	}

	void ScaleEdgeDistance( AdjacencyListType leaf1, AdjacencyListType leaf2, DistanceType scale ) {
		auto *const distanceTable = this->distanceTable;
		assert( this->distanceTableScratchpad );
		// The distance table must point at the scratchpad
		assert( distanceTable == this->distanceTableScratchpad );
		const AdjacencyListType numLeafs = this->numLeafs;
		assert( leaf1 > 0 && leaf1 < numLeafs );
		assert( leaf2 > 0 && leaf2 < numLeafs );
		distanceTable[leaf1 * numLeafs + leaf2] *= scale;
		distanceTable[leaf2 * numLeafs + leaf1] *= scale;
	}

	virtual void SaveDistanceTable() {
		// Check whether the distance table is set at all
		assert( this->distanceTable );
		// Check whether it points to the original address
		assert( this->distanceTable == this->distanceTableBackup );
		// Check whether the scratchpad is set
		assert( this->distanceTableScratchpad );
		// Make it pointing to the scratchpad
		this->distanceTable = this->distanceTableScratchpad;
		// Fill the scratchpad by the original data
		size_t memSize = this->numLeafs * this->numLeafs * sizeof( *this->distanceTable );
		memcpy( this->distanceTable, this->distanceTableBackup, memSize );
	}

	virtual void RestoreDistanceTable() {
		// Check whether the distance table is set at all
		assert( this->distanceTable );
		// Check whether it points to the scratchpad
		assert( this->distanceTable == this->distanceTableScratchpad );
		// Check whether the original address is present
		assert( this->distanceTableBackup );
		// Make the data point to the original address
		this->distanceTable = this->distanceTableBackup;
	}
};

template <typename AdjacencyListType, typename DistanceType>
class GraphBuilder: public MutableGraph<AdjacencyListType, DistanceType> {
protected:
	bool canSkipBuildTableCall { false };

	explicit GraphBuilder( int numLeafs )
		: MutableGraph<AdjacencyListType, DistanceType>( numLeafs ) {}

	virtual DistanceType ComputeEdgeDistance( int leaf1, int leaf2 ) = 0;

	virtual RefCountingAllocator<DistanceType> &TableBackupAllocator() {
		// Let's always use a ref-counting allocator for shareable data
		return RefCountingAllocatorForType<DistanceType>();
	}
	virtual TaggedAllocator<DistanceType> &TableScratchpadAllocator() {
		// Lets use base allocator intentionally to prevent sharing mutable data
		return TaggedAllocatorForType<DistanceType>();
	}
	virtual RefCountingAllocator<AdjacencyListType> &AdjacencyListsAllocator() {
		// Let's always use a ref-counting allocator for shareable data
		return RefCountingAllocatorForType<AdjacencyListType>();
	}
public:
	using TargetType = GraphLike<AdjacencyListType, DistanceType>;
protected:
	virtual void PrepareToBuild();
	virtual void BuildDistanceTable();
	virtual void BuildAdjacencyLists();

	void CheckMutualReachability( int leaf1, int leaf2 );

	template <typename T> T *TransferCheckingNullity( T **member ) {
		assert( *member );
		T *result = *member;
		*member = nullptr;
		return result;
	}

	virtual bool TryUsingGlobalGraph( TargetType *target );
public:
	/**
	 * Tries to build the graph data (or reuse data from the global graph).
	 */
	bool Build( TargetType *target = nullptr );

	void TransferOwnership( DistanceType **table, AdjacencyListType **lists, AdjacencyListType **listsOffsets ) {
		assert( this->distanceTable == this->distanceTableBackup );
		// TODO: Transfer the scratchpad as well?
		*table = TransferCheckingNullity( &this->distanceTableBackup );
		*lists = TransferCheckingNullity( &this->adjacencyListsData );
		*listsOffsets = TransferCheckingNullity( &this->adjacencyListsOffsets );
		// Prevent any reuse of this
		this->distanceTable = nullptr;
	}
};

template <typename DistanceType>
class PropagationGraphBuilder: public GraphBuilder<int, DistanceType> {
protected:
	using TargetType = GraphLike<int, DistanceType>;

	uint8_t *dirsTable { nullptr };
	const bool fastAndCoarse;

	void PrepareToBuild() override;

	RefCountingAllocator<uint8_t> &DirsAllocator() {
		return RefCountingAllocatorForType<uint8_t>();
	}

	bool TryUsingGlobalGraph( TargetType *target ) override;

	DistanceType ComputeEdgeDistance( int leaf1, int leaf2 ) override {
		trap_Error( "PropagationGraphBuilder<?, ?>::ComputeEdgeDistance() should not be called\n" );
	}
private:
	using SuperType = GraphBuilder<int, DistanceType>;
public:
	PropagationGraphBuilder( int actualNumLeafs, bool fastAndCoarse_ )
		: GraphBuilder<int, DistanceType>( actualNumLeafs ), fastAndCoarse( fastAndCoarse_ ) {}

	/**
	 * Returns an average propagation direction from leaf1 to leaf2.
	 * @param leaf1 a first leaf.
	 * @param leaf2 a second leaf.
	 * @param reuse a buffer for a valid result.
	 * @return a valid address of a direction vector on success, a null on failure
	 * @note a presence of a valid dir is symmetrical for any pair of leaves.
	 * @note a valid dir magnitude is symmetrical for any pair of leaves such that a valid dir exists for the pair.
	 */
	const float *GetDirFromLeafToLeaf( int leaf1, int leaf2, vec3_t reuse ) const {
		assert( dirsTable );
		assert( leaf1 != leaf2 );
		const int numLeafs = this->NumLeafs();
		assert( leaf1 > 0 && leaf1 < numLeafs );
		assert( leaf2 > 0 && leaf2 < numLeafs );

		// We do not store dummy rows/columns for a zero leaf so we have to shift leaf numbers and table side
		leaf1--;
		leaf2--;
		const int tableSide = numLeafs - 1;
		assert( tableSide > 0 );

		// We store only data of the upper triangle

		// *  X  X  X  X
		// -  *  X  X  X
		// -  -  *  X  X
		// -  -  -  *  X
		// -  -  -  -  *

		// Let N be the table side
		// An offset for the 0-th row data is 0
		// An offset for the 1-st row data is N - 1
		// An offset for the 2-nd row data is ( N - 1 ) + ( N - 2 )
		// An offset for the 3-rd row data is ( N - 1 ) + ( N - 2 ) + ( N - 3 )
		// An offset for the 4-th row data is ( N - 1 ) + ( N - 2 ) + ( N - 3 ) + ( N - 4 )
		// ( N - 1 ) + ( N - 2 ) + ( N - 3 ) + ( N - 4 ) = 4 * N - ( 1 + 2 + 3 + 4 ) = 4 * N - 4 * ( 1 + 4 ) / 2
		// Let R be the row number
		// Thus row offset = R * N - R * ( 1 + R ) / 2

		// Use an anti-symmetrical property of the leaf-to-leaf dir relation while trying to access the lower triangle
		float sign = 1.0f;
		if( leaf1 > leaf2 ) {
			std::swap( leaf1, leaf2 );
			sign = -1.0f;
		}

		const int rowOffset = leaf1 * tableSide - ( leaf1 * ( 1 + leaf1 ) ) / 2;
		// We iterate over cells in the upper triangle starting from i + 1 for i-th row.
		// Remember that leaf1 corresponds to row index and leaf2 corresponds to column index.
		const int indexInRow = leaf2 - leaf1 - 1;
		const uint8_t dirByte = dirsTable[rowOffset + indexInRow];
		if( dirByte != std::numeric_limits<uint8_t>::max() ) {
			::ByteToDir( dirByte, reuse );
			VectorScale( reuse, sign, reuse );
			return reuse;
		}
		return nullptr;
	}

	~PropagationGraphBuilder() override {
		TaggedAllocators::FreeUsingMetadata( dirsTable, "dirs table of builder" );
	}

	void TransferOwnership( DistanceType **distanceTable, uint8_t **dirsTable, int **lists, int **listsOffsets ) {
		SuperType::TransferOwnership( distanceTable, lists, listsOffsets );
		*dirsTable = SuperType::TransferCheckingNullity( &this->dirsTable );
	}
};

template <typename DistanceType>
class CloneableGraphBuilder: public PropagationGraphBuilder<DistanceType> {
public:
	CloneableGraphBuilder( int actualNumLeafs, bool fastAndCoarse_ )
		: PropagationGraphBuilder<DistanceType>( actualNumLeafs, fastAndCoarse_ ) {}

	/**
	 * Tries to clone the instance sharing immutable fields if it's possible.
	 * @return a non-null cloned instance on success, null on failure.
	 */
	CloneableGraphBuilder *Clone();
};

template <typename AdjacencyListType, typename DistanceType>
bool GraphBuilder<AdjacencyListType, DistanceType>::Build( TargetType *target ) {
	// Should not be called for empty graphs
	assert( this->numLeafs > 0 );

	if( !target ) {
		target = this;
	}

	if( TryUsingGlobalGraph( target ) ) {
		return true;
	}

	PrepareToBuild();

	// This flag is unset by default but some PrepareToBuild() implementations can set it
	if( !this->canSkipBuildTableCall ) {
		BuildDistanceTable();
	}

	BuildAdjacencyLists();
	return true;
}

/**
 * This is a helper for {@PropagationGraphBuilder::PrepareToBuild()} algorithms.
 * Putting these algorithms in a separate class serves two purposes:
 */
class LeafToLeafDirBuilder {
	trace_t trace;
	std::minstd_rand randomnessEngine;
	vec3_t leafCenters[2];
	vec3_t leafDimensions[2];
	vec3_t leafPoints[2];
	const int maxAdditionalAttempts;

	/**
	 * This is a helper that tries to pick a random point within a leaf bounds.
	 * A picking might fail for degenerate leaves of some kind.
	 * @param leafNum an actual leaf num in the collision world.
	 * @param storageIndex an index of internal storage for first or second leaf (0 or 1)
	 * @return false if a point picking has failed. A dir building should be interrupted in this case.
	 */
	bool PrepareTestedPointForLeaf( int leafNum, int storageIndex );
public:
	explicit LeafToLeafDirBuilder( bool fastAndCoarse_, int numLeafs_ )
		: maxAdditionalAttempts( fastAndCoarse_ ? ( numLeafs_ > 1000 ? 0 : 5 ) : 24 ) {
		// Note: we actually are cutting off additional attempts for complex maps.
		// This is a hack to prevent computational explosion until more efficient algorithms are introduced
		// (switching from Dijkstra to bidirectional Dijkstra was good but not sufficient)
	}

	/**
	 * Tries to test propagation for a given leaves pair.
	 * @param leaf1 a first leaf.
	 * @param leaf2 a second leaf.
	 * @param resultDir a buffer for a propagation dir.
	 * @return an average distance for propagation between leaves on success, infinity on failure.
	 */
	float Build( int leaf1, int leaf2, vec3_t resultDir );
};

bool LeafToLeafDirBuilder::PrepareTestedPointForLeaf( int leafNum, int storageIndex ) {
	float *const point = leafPoints[storageIndex];
	const auto randomShift = (float)std::minstd_rand::min();
	const auto randomScale = 1.0f / ( std::minstd_rand::max() - randomShift );
	// Try 6 attempts to pick a random point within a leaf bounds
	for( int attemptNum = 0; attemptNum < 6; ++attemptNum ) {
		for( int i = 0; i < 3; ++i ) {
			point[i] = leafCenters[storageIndex][i];
			float random01 = randomScale * ( randomnessEngine() - randomShift );
			point[i] += -0.5f + random01 * leafDimensions[storageIndex][i];
		}
		if( trap_PointLeafNum( point ) == leafNum ) {
			return true;
		}
	}
	return false;
}

float LeafToLeafDirBuilder::Build( int leaf1, int leaf2, vec3_t resultDir ) {
	if( !trap_LeafsInPVS( leaf1, leaf2 ) ) {
		return std::numeric_limits<float>::infinity();
	}

	const vec3_t *const bounds[2] = { trap_GetLeafBounds( leaf1 ), trap_GetLeafBounds( leaf2 ) };

	for( int i = 0; i < 2; ++i ) {
		// Get dimensions
		VectorSubtract( bounds[i][1], bounds[i][0], leafCenters[i] );
		VectorCopy( leafCenters[i], leafDimensions[i] );
		// Get half-dimensions
		VectorScale( leafCenters[i], 0.5f, leafCenters[i] );
		// Add mins
		VectorAdd( leafCenters[i], bounds[i][0], leafCenters[i] );
	}

	// Prepare for adding dir contributions
	VectorClear( resultDir );
	bool hasContributingDirs = false;

	// Cast a ray from a leaf center to another leaf center.
	// Do not test whether these centers really belong to a leaf
	// (we remember this happening a lot for (almost) degenerate leaves while computing LeafPropsCache).
	trap_Trace( &trace, leafCenters[0], leafCenters[1], vec3_origin, vec3_origin, MASK_SOLID );
	if( trace.fraction == 1.0f ) {
		// Add center-to-center dir contribution.
		VectorSubtract( leafCenters[1], leafCenters[0], resultDir );
		float squareLength = VectorLengthSquared( resultDir );
		if( squareLength > 1 ) {
			// Give this dir a 3x greater weight
			float scale = 3.0f / std::sqrt( squareLength );
			VectorScale( resultDir, scale, resultDir );
			hasContributingDirs = true;
		}
	}

	for( int attemptNum = 0; attemptNum < this->maxAdditionalAttempts; ++attemptNum ) {
		int leaves[2] = { leaf1, leaf2 };
		for( int j = 0; j < 2; ++j ) {
			// For every leaf try picking random points within leaf bounds.
			// Stop doing attempts immediately on failure
			// (we are very likely have met another kind of a degenerate leaf).
			// TODO: We can try reusing picked leaf points from LeafPropsCache
			if( !this->PrepareTestedPointForLeaf( leaves[j], j ) ) {
				goto done;
			}
		}

		trap_Trace( &trace, leafPoints[0], leafPoints[1], vec3_origin, vec3_origin, MASK_SOLID );
		if( trace.fraction != 1.0f ) {
			continue;
		}

		vec3_t tmp;
		// Try adding contribution of dir from first to second leaf
		VectorSubtract( leafPoints[1], leafPoints[0], tmp );
		const float squareDistance = VectorLengthSquared( tmp );
		if( squareDistance < 1 ) {
			continue;
		}

		const float scale = 1.0f / std::sqrt( squareDistance );
		VectorScale( tmp, scale, tmp );
		VectorAdd( tmp, resultDir, resultDir );
		hasContributingDirs = true;
	}

done:
	if( !hasContributingDirs ) {
		return std::numeric_limits<float>::infinity();
	}

	// Must always produce a valid normalized dir once we have reached here
	VectorNormalize( resultDir );
	// Check normalization
	assert( std::abs( std::sqrt( VectorLengthSquared( resultDir ) ) - 1.0f ) < 0.2f );
	// Always return a plain distance between leaf centers.
	return std::sqrt( DistanceSquared( leafCenters[0], leafCenters[1] ) );
}

// TODO: Lift ByteToDirTable to outer scope

static const vec3_t byteToDirNormals[] = {
#include "../gameshared/anorms.h"
};

class DirToByteTable {
	enum: uint8_t { SIZE = sizeof( byteToDirNormals ) / sizeof( *byteToDirNormals ) };

	// Works quite good.
	// Choosing a proper hash function that groups together similar directories is what that matter.
	enum { NUM_BINS = SIZE };
	uint8_t bins[NUM_BINS];
	uint8_t next[SIZE];

	enum: uint8_t {  NULL_LINK = std::numeric_limits<uint8_t>::max() };
	// Ensure we can use 255 and 254 to indicate something else
	static_assert( SIZE < std::numeric_limits<uint8_t>::max(), "" );

	// The hit ratio (a success rate of GetFirstHashedFit()) is very good, something about 90 % or even more
	static inline uint32_t Hash( const vec3_t v ) {
		auto u0 = (uint32_t)( fabsf( v[0] ) * 3 );
		auto u1 = (uint32_t)( fabsf( v[1] ) * 3 );
		auto u2 = (uint32_t)( fabsf( v[2] ) * 3 );
		return u0 * 64 + u1 * 8 + u2;
	}

	DirToByteTable() {
		// MSVC has troubles compiling std::fill_n() for NULL_LINK (?) type and using memset() is error-prone
		for( int i = 0; i < NUM_BINS; ++i ) {
			bins[i] = NULL_LINK;
		}

		for( unsigned i = 0; i < SIZE; ++i ) {
			int binIndex = Hash( ::byteToDirNormals[i] ) % NUM_BINS;
			int oldHead = bins[binIndex];
			// Link old bin head (or "null") as next for the newly added entry
			next[i] = (uint8_t)oldHead;
			// Link i-th entry to bin at bin index
			bins[binIndex] = (uint8_t)i;
		}
	}

	int GetFirstHashedFit( const vec3_t v ) const {
		int binIndex = Hash( v ) % NUM_BINS;
		const auto *normals = ::byteToDirNormals;
		for( int num = bins[binIndex]; num != NULL_LINK; ) {
			const auto *n = normals[num];
			if( DotProduct( v, n ) > 0.95f ) {
				return num;
			}
			num = next[num];
		}
		return -1;
	}

	int ScanForFirstFit( const vec3_t v ) const {
		const auto *normals = ::byteToDirNormals;
		for( int i = 0; i < SIZE; ++i ) {
			const auto *n = normals[i];
			if( DotProduct( v, n ) > 0.95f ) {
				return i;
			}
		}
		return -1;
	}

	static const DirToByteTable instance;
public:
	static int DirToByte( const vec3_t dir ) {
		// Try getting a value in the same hash bin that is good enough
		int byte = instance.GetFirstHashedFit( dir );
		if( byte >= 0 ) {
			return byte;
		}

		byte = instance.ScanForFirstFit( dir );
		if( byte >= 0 ) {
			return byte;
		}

		// Fallback to the default implementation. Should happen extremely rarely.
		return ::DirToByte( const_cast<float *>( dir ) );
	}

	static bool IsValidDirByte( int byte ) {
		return (unsigned)byte < SIZE;
	}
};

const DirToByteTable DirToByteTable::instance;

template <typename DistanceType>
void PropagationGraphBuilder<DistanceType>::PrepareToBuild() {
	SuperType::PrepareToBuild();

	assert( this->distanceTable );
	const int numLeafs = this->numLeafs;

	LeafToLeafDirBuilder dirBuilder( fastAndCoarse, numLeafs );

	vec3_t dir;
	const int dirsTableSide = numLeafs - 1;
	dirsTable = DirsAllocator().Alloc( dirsTableSide * dirsTableSide - dirsTableSide, "dirs table" );
	uint8_t *dirsDataPtr = &dirsTable[0];
	for( int i = 1; i < numLeafs; ++i ) {
		for( int j = i + 1; j < numLeafs; ++j ) {
			// Don't be confused by "float" type as leaf-to-leaf computations are always performed in single precision
			const float distance = dirBuilder.Build( i, j, dir );
			this->distanceTable[i * numLeafs + j] = this->distanceTable[j * numLeafs + i] = distance;
			if( !std::isfinite( distance ) ) {
				*dirsDataPtr++ = std::numeric_limits<uint8_t>::max();
				// Check immediately
				assert( !GetDirFromLeafToLeaf( i, j, dir ) );
				continue;
			}
			*dirsDataPtr++ = (uint8_t)DirToByteTable::DirToByte( dir );
			// Check immediately
			assert( GetDirFromLeafToLeaf( i, j, dir ) );
		}
	}

	// We have already set distance table values
	this->canSkipBuildTableCall = true;
}

/**
 * This is extracted to a separate function as it was used in a code that was rejected before committing.
 * Might be useful in future.
 */
template <typename ExistingType, typename AllocatorElemType>
static AllocatorElemType *ConvertFloatArray( const ExistingType *existing, int numVectorElems, const char *logTag ) {
	assert( existing );

	// Using a ref-counting allocator is either mandatory or just won't hurt
	auto *const result = RefCountingAllocatorForType<AllocatorElemType>().Alloc( numVectorElems, logTag );

	// Convert using flattened views of data.
	// This is a more generic approach and yields better code in all builds.
	const int numScalarElems = ( numVectorElems * sizeof( ExistingType ) ) / sizeof( float );
	const auto *const __restrict existingView = (const float *)existing;
	auto *const __restrict resultView = (double *)result;
	for( int i = 0; i < numScalarElems; ++i ) {
		// Expand float to double...
		resultView[i] = existingView[i];
	}

	return result;
}

/**
 * Tries to reuse the distance table of the global leafs graph
 * either just by sharing it or performing data conversion without expensive computations.
 * An appropriate version is selected for {@code DistanceType} by template substitution rules.
 */
template <typename DistanceType> DistanceType *ReuseGlobalDistanceTable( int numLeafs );

/**
 * A specialization of {@code ReuseGlobalDistanceTable<?>( int )} that shares the global leafs graph distance table.
 */
template <> float *ReuseGlobalDistanceTable<float>( int ) {
	float *existingTable = CachedLeafsGraph::Instance()->distanceTable;
	return RefCountingAllocatorForType<float>().AddRef( existingTable, "global distance table" );
}

/**
 * A specialization of {@code ReuseGlobalDistanceTable<?>( int )} that builds a table of doubles
 * by converting data of the global leafs graph distnace table.
 */
template <> double *ReuseGlobalDistanceTable<double>( int numLeafs ) {
	float *existingTable = CachedLeafsGraph::Instance()->distanceTable;
	return ConvertFloatArray<float, double>( existingTable, numLeafs * numLeafs, "global distance table" );
}

template <typename DistanceType>
bool PropagationGraphBuilder<DistanceType>::TryUsingGlobalGraph( TargetType *target ) {
	if( !SuperType::TryUsingGlobalGraph( target ) ) {
		return false;
	}

	auto *existingTable = CachedLeafsGraph::Instance()->dirsTable;
	this->dirsTable = RefCountingAllocatorForType<uint8_t>().AddRef( existingTable, "dirs table" );
	return true;
}

template <typename AdjacencyListType, typename DistanceType>
void GraphBuilder<AdjacencyListType, DistanceType>::PrepareToBuild() {
	int numTableCells = this->NumLeafs() * this->NumLeafs();
	// We can't allocate "backup" and "scratchpad" in the same chunk
	// as we want to be able to share the immutable table data ("backup")
	// Its still doable by providing a custom allocator.
	this->distanceTableBackup = TableBackupAllocator().Alloc( numTableCells, "distance table backup" );
	this->distanceTableScratchpad = TableScratchpadAllocator().Alloc( numTableCells, "distance table scratchpad" );
	// Set the distance table as it is expected for the first SaveDistanceTable() call
	this->distanceTable = this->distanceTableBackup;
}

template <typename AdjacencyListType, typename DistanceType>
void GraphBuilder<AdjacencyListType, DistanceType>::CheckMutualReachability( int leaf1, int leaf2 ) {
	assert( leaf1 != leaf2 );
	DistanceType direct = this->EdgeDistance( leaf1, leaf2 );
	// Must be either a valid positive distance or an infinity
	assert( direct > 0 );
	DistanceType reverse = this->EdgeDistance( leaf2, leaf1 );
	// Takes infinity into account as well
	assert( direct == reverse );
	// Avoid warnings in release builds
	(void)direct, (void)reverse;
}

template <typename AdjacencyListType, typename DistanceType>
void GraphBuilder<AdjacencyListType, DistanceType>::BuildDistanceTable() {
	for( int i = 1; i < this->numLeafs; ++i ) {
		for( int j = i + 1; j < this->numLeafs; ++j ) {
			this->SetEdgeDistance( i, j, this->ComputeEdgeDistance( i, j ) );
#ifndef PUBLIC_BUILD
			this->CheckMutualReachability( i, j );
#endif
		}
	}
}

template <typename AdjacencyListType, typename DistanceType>
void GraphBuilder<AdjacencyListType, DistanceType>::BuildAdjacencyLists() {
	const int numLeafs = this->numLeafs;
	const auto *distanceTable = this->distanceTable;
	size_t totalNumCells = 0;
	for( int i = 1; i < numLeafs; ++i ) {
		int rowOffset = i * numLeafs;
		for( int j = 1; j < i; ++j ) {
			if( std::isfinite( distanceTable[rowOffset + j] ) ) {
				totalNumCells++;
			}
		}
		for( int j = i + 1; j < numLeafs; ++j ) {
			if( std::isfinite( distanceTable[rowOffset + j] ) ) {
				totalNumCells++;
			}
		}
	}

	// A first additional cell for a leaf is for a size "prefix" of adjacency list
	// A second additional cell is for offset of the adjacency list in the compactified data
	totalNumCells += 2 * numLeafs;
	AdjacencyListType *mem = AdjacencyListsAllocator().Alloc( (int)totalNumCells, "adjacency data" );
	auto *const adjacencyListsData = this->adjacencyListsData = mem;
	auto *const adjacencyListsOffsets = this->adjacencyListsOffsets = mem + ( totalNumCells - numLeafs );

	AdjacencyListType *dataPtr = adjacencyListsData;
	// Write a zero-length list for the zero leaf
	*dataPtr++ = 0;
	adjacencyListsOffsets[0] = 0;

	for( int i = 1; i < numLeafs; ++i ) {
		int rowOffset = i * numLeafs;
		// Save a position of the list length
		AdjacencyListType *const listLengthRef = dataPtr++;
		for( int j = 1; j < i; ++j ) {
			if( std::isfinite( distanceTable[rowOffset + j] ) ) {
				*dataPtr++ = j;
			}
		}
		for( int j = i + 1; j < numLeafs; ++j ) {
			if( std::isfinite( distanceTable[rowOffset + j] ) ) {
				*dataPtr++ = j;
			}
		}
		adjacencyListsOffsets[i] = (AdjacencyListType)( listLengthRef - adjacencyListsData );
		*listLengthRef = (AdjacencyListType)( dataPtr - listLengthRef - 1 );
	}
}

template <typename DistanceType>
struct HeapEntry {
	DistanceType distance;
	DistanceType heapCost;
	int leafNum;

	HeapEntry( int leafNum_, DistanceType distance_, DistanceType heapCost_ )
		: distance( distance_ ), heapCost( heapCost_ ), leafNum( leafNum_ ) {}

	bool operator<( const HeapEntry &that ) const {
		// std:: algorithms use a max-heap
		return heapCost > that.heapCost;
	}
};

/**
 * A specialized updates heap optimized for path-finding usage patterns.
 */
template <typename DistanceType>
class UpdatesHeap {
	HeapEntry<DistanceType> *buffer;
	int size { 0 };
	int capacity { 1024 + 512 };
public:
	UpdatesHeap() {
		buffer = (HeapEntry<DistanceType> *)S_Malloc( sizeof( HeapEntry<DistanceType> ) * capacity );
	}

	~UpdatesHeap() {
		S_Free( buffer );
	}

	void Clear() {
		size = 0;
	}

	/**
	 * Constructs a new {@code HeapEntry} in place and adds it to the heap.
	 * @param leaf a forwarded parameter of {@code HeapEntry()} constructor.
	 * @param distance a forwarded parameter of {@code HeapEntry()} constructor.
	 * @param heapCost a forwarded parameter of {@code HeapEntry()} constructor.
	 */
	void Push( int leaf, DistanceType distance, DistanceType heapCost ) {
		new( buffer + size++ )HeapEntry<DistanceType>( leaf, distance, heapCost );
		std::push_heap( buffer, buffer + size );
	}

	bool IsEmpty() const {
		return !size;
	}

	DistanceType BestDistance() const {
		return buffer[0].distance;
	}

	/**
	 * Pops a best heap entry.
	 * The returned value is valid until next {@code PrepareToAdd()} call.
	 * @return a reference to the newly popped entry.
	 */
	const HeapEntry<DistanceType> &PopInPlace() {
		std::pop_heap( buffer, buffer + size );
		return buffer[--size];
	}

	/**
	 * Reserves buffer capacity for items that are about to be added.
	 * @param atMost a maximum number of newly added items.
	 */
	void ReserveForAddition( int atMost ) {
		if( size + atMost <= capacity ) {
			return;
		}
		capacity = ( 4 * ( size + atMost ) ) / 3;
		auto *const oldBuffer = buffer;
		buffer = (HeapEntry<DistanceType> *)S_Malloc( sizeof( HeapEntry<DistanceType> ) * capacity );
		memcpy( buffer, oldBuffer, sizeof( HeapEntry<DistanceType> ) * size );
		S_Free( oldBuffer );
	}
};

template <typename DistanceType>
struct VertexUpdateStatus {
	DistanceType distance[2];
	int32_t parentLeaf[2];
	bool isVisited[2];
};

template <typename> class PathFinder;

template <typename DistanceType>
class PathReverseIterator {
	template <typename> friend class PathFinder;
	PathFinder<DistanceType> *const parent;
	int leafNum { std::numeric_limits<int>::min() };
	const int listIndex;

	PathReverseIterator( PathFinder<DistanceType> *parent_, int listIndex_ )
		: parent( parent_ ), listIndex( listIndex_ ) {}

	void ResetWithLeaf( int leafNum_ ) {
		assert( leafNum_ > 0 );
		this->leafNum = leafNum_;
	}
public:
	bool HasNext() const {
		return leafNum > 0 && parent->updateStatus[leafNum].parentLeaf[listIndex] > 0;
	}

	int LeafNum() const { return leafNum; }

	void Next() {
		assert( HasNext() );
		leafNum = parent->updateStatus[leafNum].parentLeaf[listIndex];
	}
};

template <typename DistanceType>
class PathFinder {
	template <typename> friend class PathReverseIterator;
	template <typename> friend class PropagationTableBuilder;

	/**
	 * An euclidean leaf-to-leaf distance table supplied by a parent
	 */
	const float *const euclideanDistanceTable;

	PropagationGraphBuilder<DistanceType> &graphBuilder;

	VertexUpdateStatus<DistanceType> *updateStatus { nullptr };

	UpdatesHeap<DistanceType> heaps[2];

	using IteratorType = PathReverseIterator<DistanceType>;

	IteratorType tmpDirectIterator;
	IteratorType tmpReverseIterator;

	float GetEuclideanDistance( int leaf1, int leaf2 ) const {
		return euclideanDistanceTable[leaf1 * graphBuilder.NumLeafs() + leaf2];
	}
public:
	explicit PathFinder( const float *euclideanDistanceTable_, PropagationGraphBuilder<DistanceType> &graph_ )
		: euclideanDistanceTable( euclideanDistanceTable_ )
		, graphBuilder( graph_ )
		, tmpDirectIterator( this, 0 )
		, tmpReverseIterator( this, 1 ) {
		size_t memSize = graph_.NumLeafs() * sizeof( VertexUpdateStatus<DistanceType> );
		updateStatus = (VertexUpdateStatus<DistanceType> *)::S_Malloc( memSize );
	}

	~PathFinder() {
		if( updateStatus ) {
			S_Free( updateStatus );
		}
	}

	/**
	 * Finds a path from a leaf to a leaf.
	 * The search algorithm is intended to be 2-directional
	 * and uniformly interleaving from direct to reverse propagation.
	 * Ordering of leaves does not really matter as the graph is undirected.
	 * @param fromLeaf a first leaf.
	 * @param toLeaf a second leaf.
	 * @param direct set to an address of a direct algorithm turn iterator on success.
	 * @param reverse set to an address of a reverse algorithm turn iterator on success.
	 * @return a best distance on success, an infinity on failure.
	 * @note returned iterators must be traversed backwards to get the algoritm turn start point (from- or to-leaf).
	 * These iterators are not assumed to contain complete direct/reverse path.
	 * They point to algorithm temporaries.
	 * However their combination is intended to represent an entire path.
	 * A last valid leaf during iteration matches {@code fromLeaf} and {@code toLeaf} accordingly.
	 */
	DistanceType FindPath( int fromLeaf, int toLeaf, IteratorType **direct, IteratorType **reverse );
};

template <typename DistanceType>
DistanceType PathFinder<DistanceType>::FindPath( int fromLeaf,
												 int toLeaf,
												 IteratorType **direct,
												 IteratorType **reverse ) {
	// A-star hinting targets for each turn
	const int turnTargetLeaf[2] = { toLeaf, fromLeaf };

	for( int i = 0, end = graphBuilder.NumLeafs(); i < end; ++i ) {
		auto *status = updateStatus + i;
		for( int turn = 0; turn < 2; ++turn ) {
			status->distance[turn] = std::numeric_limits<DistanceType>::infinity();
			status->parentLeaf[turn] = -1;
			status->isVisited[turn] = false;
		}
	}

	heaps[0].Clear();
	heaps[1].Clear();

	updateStatus[fromLeaf].distance[0] = DistanceType( 0 );
	heaps[0].Push( fromLeaf, DistanceType( 0 ), GetEuclideanDistance( fromLeaf, toLeaf ) );
	updateStatus[toLeaf].distance[1] = DistanceType( 0 );
	heaps[1].Push( toLeaf, DistanceType( 0 ), GetEuclideanDistance( toLeaf, fromLeaf ) );

	int bestLeaf = -1;
	auto bestDistanceSoFar = std::numeric_limits<DistanceType>::infinity();
	while( !heaps[0].IsEmpty() && !heaps[1].IsEmpty() ) {
		for( int turn = 0; turn < 2; ++turn ) {
			if( heaps[0].BestDistance() + heaps[1].BestDistance() >= bestDistanceSoFar ) {
				assert( bestLeaf > 0 );
				// Check whether this leaf has been really touched by direct and reverse algorithm turns
				assert( updateStatus[bestLeaf].parentLeaf[0] >= 0 );
				assert( updateStatus[bestLeaf].parentLeaf[1] >= 0 );
				tmpDirectIterator.ResetWithLeaf( bestLeaf );
				*direct = &tmpDirectIterator;
				tmpReverseIterator.ResetWithLeaf( bestLeaf );
				*reverse = &tmpReverseIterator;
				return bestDistanceSoFar;
			}

			auto *const activeHeap = &heaps[turn];

			const HeapEntry<DistanceType> &entry = activeHeap->PopInPlace();
			// Save these values immediately as ReserveForAddition() call might make accessing the entry illegal.
			const int entryLeafNum = entry.leafNum;
			const double entryDistance = updateStatus[entryLeafNum].distance[turn];

			updateStatus[entryLeafNum].isVisited[turn] = true;

			// Now scan all adjacent vertices
			const auto *const adjacencyList = graphBuilder.AdjacencyList( entryLeafNum ) + 1;
			const auto listSize = adjacencyList[-1];
			activeHeap->ReserveForAddition( listSize );
			for( int i = 0; i < listSize; ++i ) {
				const auto leafNum = adjacencyList[i];
				auto *const status = &updateStatus[leafNum];
				// We do not have to re-check already visited nodes for an euclidean heuristic
				if( status->isVisited[turn] ) {
					continue;
				}
				DistanceType edgeDistance = graphBuilder.EdgeDistance( entryLeafNum, leafNum );
				DistanceType relaxedDistance = edgeDistance + entryDistance;
				if( status->distance[turn] <= relaxedDistance ) {
					continue;
				}

				DistanceType otherDistance = status->distance[( turn + 1 ) & 1];
				if( otherDistance + relaxedDistance < bestDistanceSoFar ) {
					bestLeaf = leafNum;
					bestDistanceSoFar = otherDistance + relaxedDistance;
				}

				status->distance[turn] = relaxedDistance;
				status->parentLeaf[turn] = entryLeafNum;

				DistanceType euclideanDistance = GetEuclideanDistance( leafNum, turnTargetLeaf[turn] );
				activeHeap->Push( leafNum, relaxedDistance, relaxedDistance + euclideanDistance );
			}
		}
	}

	return std::numeric_limits<DistanceType>::infinity();
}

template <typename> class PropagationTableBuilder;

template <typename DistanceType>
class PropagationBuilderTask: public ParallelComputationHost::PartialTask {
	template <typename> friend class PropagationTableBuilder;
public:
	using PropagationProps = PropagationTable::PropagationProps;
	using IteratorType = PathReverseIterator<DistanceType>;
	using ParentBuilderType = PropagationTableBuilder<DistanceType>;
	using GraphType = CloneableGraphBuilder<DistanceType>;
	using PathFinderType = PathFinder<DistanceType>;
private:

	ParentBuilderType *const parent;
	PropagationProps *const table;

	GraphType *graphInstance { nullptr };
	PathFinderType *pathFinderInstance { nullptr };
	int *tmpLeafNums { nullptr };
	const int numLeafs;
	int leafsRangeBegin { -1 };
	int leafsRangeEnd { -1 };
	int total { -1 };
	int executed { 0 };
	int lastReportedProgress { 0 };
	int executedAtLastReport { 0 };
	const bool fastAndCoarse;

	PropagationBuilderTask( ParentBuilderType *parent_, int numLeafs_ );

	~PropagationBuilderTask() override;

	void Exec() override;

	void ComputePropsForPair( int leaf1, int leaf2 );

	void BuildInfluxDirForLeaf( float *allocatedDir, const int *leafsChain, int numLeafsInChain );

	void BuildInfluxDirForLeaf( float *allocatedDir, const int *leafsChainBegin, const int *leafsChainEnd ) {
		assert( leafsChainEnd > leafsChainBegin );
		// Sanity check
		assert( leafsChainEnd - leafsChainBegin < ( 1 << 20 ) );
		BuildInfluxDirForLeaf( allocatedDir, leafsChainBegin, (int)( leafsChainEnd - leafsChainBegin ) );
	}

	/**
	 * Unwinds a {@code PathReverseIterator} writing leaf numbers to a linear buffer.
	 * The buffer is assumed to be capable to store a leaves chain of maximum possible length for the current graph.
	 * Scales graph edges defined by these leaf numbers at the same time.
	 * @param iterator an iterator that represents intermediate results of path-finding.
	 * @param arrayEnd an end of the buffer range. Leaf numbers will be written before this address.
	 * @param scale a weight scale for path edges
	 * @return a new range begin for the buffer (that is less than the {@code arrayEnd}
	 */
	int *UnwindScalingWeights( IteratorType *iterator, int *arrayEnd, DistanceType scale );
	/**
	 * Unwinds a {PathReverseIterator} writing leaf numbers to a linear buffer.
	 * The buffer is assumed to be capable to store a leaves chain of maximum possible length for the current graph.
	 * @param iterator an iterator that represents intermediate results of path-finding.
	 * @param arrayEnd an end of the buffer range. Leaf numbers will be written before this address.
	 * @return a new range begin for the buffer (that is less than the {@code arrayEnd}
	 */
	int *Unwind( IteratorType *iterator, int *arrayEnd );

	/**
	 * Builds a propagation path between given leaves.
	 * As the reachability presence and distance relations are symmetrical,
	 * an output is produced for direct and reverse path simultaneously.
	 * However influx dirs for first and second dir are not related at all
	 * (an influx dir for the second leaf is not an inversion of an influx dir for the first one).
	 * @param leaf1 a first leaf, must be distinct from the second one.
	 * @param leaf2 a second leaf, must be distinct from the first one.
	 * @param _1to2 a buffer for resulting propagation dir from first to second leaf.
	 * The result contains an average dir of sound flowing into second leaf while a source is placed at the first one.
	 * @param _2to1 a buffer for resulting propagation dir from second to first leaf.
	 * The result contains an average dir of sound flowing into first leaf while a source is placed at the second one.
	 * @param distance a distance of the best met temporary path returned as an out parameter.
	 * @return true if a path has been managed to be built successfully.
	 */
	bool BuildPropagationPath( int leaf1, int leaf2, vec3_t _1to2, vec3_t _2to1, DistanceType *distance );
};

static inline void ComputeLeafCenter( int leaf, vec3_t result ) {
	const vec3_t *bounds = trap_GetLeafBounds( leaf );
	VectorSubtract( bounds[1], bounds[0], result );
	VectorScale( result, 0.5f, result );
	VectorAdd( bounds[0], result, result );
}

static inline float ComputeLeafToLeafDistance( int leaf1, int leaf2 ) {
	vec3_t center1, center2;
	ComputeLeafCenter( leaf1, center1 );
	ComputeLeafCenter( leaf2, center2 );
	return std::sqrt( DistanceSquared( center1, center2 ) );
}

static void BuildLeafEuclideanDistanceTable( float *table, int numLeafs ) {
	for( int i = 1; i < numLeafs; ++i ) {
		for( int j = i + 1; j < numLeafs; ++j ) {
			float distance = ComputeLeafToLeafDistance( i, j );
			table[i * numLeafs + j] = table[j * numLeafs + i] = (uint16_t)distance;
		}
	}
}

template <typename DistanceType>
class PropagationTableBuilder {
	template <typename> friend class PropagationBuilderTask;

	/**
	 * An euclidean distance table for leaves
	 * @todo using short values is sufficient for the majority of maps
	 */
	float *const euclideanDistanceTable { nullptr };

	using PropagationProps = PropagationTable::PropagationProps;
	using PathFinderType = PathFinder<DistanceType>;
	using TaskType = PropagationBuilderTask<DistanceType>;

	CloneableGraphBuilder<DistanceType> graphBuilder;
	PathFinderType pathFinder;

	PropagationProps *table { nullptr };
	struct qmutex_s *progressLock { nullptr };
	std::atomic_int executedWorkload { 0 };
	std::atomic_int lastShownProgress { 0 };
	int totalWorkload { -1 };

	const bool fastAndCoarse;

	/**
	 * Adds a task progress to an overall progress.
	 * @param taskWorkloadDelta a number of workload units since last task progress report.
	 * A workload unit is a computation of {@code PropagationProps} for a pair of leafs.
	 * @note use this sparingly, only if "shown" progress of a task (progress percents) is changed.
	 * This is not that cheap to call.
	 */
	void AddTaskProgress( int taskWorkloadDelta );

	void ValidateJointResults();

#ifndef _MSC_VER
	void ValidationError( const char *format, ... )
		__attribute__( ( format( printf, 2, 3 ) ) ) __attribute__( ( noreturn ) );
#else
	__declspec( noreturn ) void ValidationError( _Printf_format_string_ const char *format, ... );
#endif
public:
	PropagationTableBuilder( int actualNumLeafs, bool fastAndCoarse_ )
		: euclideanDistanceTable( (float *)S_Malloc( actualNumLeafs * actualNumLeafs * sizeof( float ) ) )
		, graphBuilder( actualNumLeafs, fastAndCoarse_ )
		, pathFinder( euclideanDistanceTable, graphBuilder )
		, fastAndCoarse( fastAndCoarse_ ) {
		assert( executedWorkload.is_lock_free() );

		if( euclideanDistanceTable ) {
			BuildLeafEuclideanDistanceTable( euclideanDistanceTable, actualNumLeafs );
		}
	}

	~PropagationTableBuilder();

	bool Build();

	inline PropagationProps *ReleaseOwnership();
};

template <typename DistanceType>
typename PropagationTableBuilder<DistanceType>::PropagationProps *PropagationTableBuilder<DistanceType>::ReleaseOwnership() {
	assert( table );
	auto *result = table;
	table = nullptr;
	return result;
}

template <typename DistanceType>
PropagationTableBuilder<DistanceType>::~PropagationTableBuilder() {
	if( table ) {
		S_Free( table );
	}
	if( progressLock ) {
		trap_Mutex_Destroy( &progressLock );
	}
	if( euclideanDistanceTable ) {
		S_Free( euclideanDistanceTable );
	}
}

class QMutexLock {
	struct qmutex_s *mutex;
public:
	explicit QMutexLock( struct qmutex_s *mutex_ ): mutex( mutex_ ) {
		assert( mutex );
		trap_Mutex_Lock( mutex );
	}
	~QMutexLock() {
		trap_Mutex_Unlock( mutex );
	}
};

template <typename DistanceType>
void PropagationTableBuilder<DistanceType>::AddTaskProgress( int taskWorkloadDelta ) {
	assert( taskWorkloadDelta > 0 );
	assert( totalWorkload > 0 && "The total workload value has not been set" );

	QMutexLock lock( progressLock );

	int newWorkload = executedWorkload.fetch_add( taskWorkloadDelta, std::memory_order_seq_cst );
	const auto newProgress = (int)( ( 100.0f / (float)totalWorkload ) * newWorkload );
	if( newProgress == lastShownProgress.load( std::memory_order_acquire ) ) {
		return;
	}

	lastShownProgress.store( newProgress, std::memory_order_release );
	Com_Printf( "Computing a sound propagation table... %2d%%\n", newProgress );
}

template <typename DistanceType>
bool PropagationTableBuilder<DistanceType>::Build() {
	// If the euclidean distance table allocation has failed
	if( !euclideanDistanceTable ) {
		return false;
	}

	progressLock = trap_Mutex_Create();
	if( !progressLock ) {
		return false;
	}

	if( !graphBuilder.Build() ) {
		return false;
	}

	const int numLeafs = graphBuilder.NumLeafs();
	const size_t tableSizeInBytes = numLeafs * numLeafs * sizeof( PropagationProps );
	// Use S_Malloc() for that as the table is transferred to PropagationTable itself
	table = (PropagationProps *)S_Malloc( tableSizeInBytes );
	if( !table ) {
		return false;
	}

	memset( table, 0, tableSizeInBytes );

	// Right now the computation host lifecycle should be limited only to scope where actual computations occur.
	ComputationHostLifecycleHolder computationHostLifecycleHolder;

	auto *const computationHost = computationHostLifecycleHolder.Instance();
	const int numTasks = computationHost->SuggestNumberOfTasks();

	// First try creating tasks
	int actualNumTasks = 0;
	// Up to 32 parallel tasks are supported...
	// We are probably going to exceed memory capacity trying to create more tasks...
	PropagationBuilderTask<DistanceType> *submittedTasks[32];
	for( int i = 0; i < std::min( 32, numTasks ); ++i ) {
		// TODO: Use just malloc()
		void *const objectMem = S_Malloc( sizeof( TaskType ) );
		if( !objectMem ) {
			break;
		}

		auto *const task = new( objectMem )TaskType( this, numLeafs );
		// A task gets an ownership over the clone
		task->graphInstance = graphBuilder.Clone();
		if( !task->graphInstance ) {
			break;
		}

		// TODO: Use just malloc()
		void *const pathFinderMem = S_Malloc( sizeof( PathFinderType ) );
		if( !pathFinderMem ) {
			break;
		}

		// A task gets an ownership over the instance
		task->pathFinderInstance = new( pathFinderMem )PathFinderType( euclideanDistanceTable, *task->graphInstance );

		// The "+1" part is not mandatory but we want a range "end"
		// to always have a valid address in address space.
		// The task gets an ownership over this chunk of memory
		task->tmpLeafNums = (int *)S_Malloc( 2 * ( numLeafs + 1 ) * sizeof( int ) );

		// Transfer ownership over the task to the host.
		// It will be released, sooner or later, regardless of TryAddTask() return value.
		if( !computationHost->TryAddTask( task ) ) {
			break;
		}

		submittedTasks[actualNumTasks++] = task;
	}

	if( !actualNumTasks ) {
		Com_Printf( S_COLOR_RED "Unable to create/enqueue at least a single PropagationBuilderTask\n" );
		return false;
	}

	// Set the total number of workload units
	// (a computation of props for a pair of leafs is a workload unit)
	this->totalWorkload = ( numLeafs - 1 ) * ( numLeafs - 2 ) / 2;

	// We cannot assign workload ranges until we know the actual number of tasks we have created

	// This is a workload for every task that is assumed to be same.
	// We use a variable assigned range (that affects the processed matrix "field" area)
	// so the "area" is (almost) the the same for every task.
	// Note that a task execution time may still vary due to different topology of processed graph parts
	// but this should give a close match to an ideal workload distribution anyway.
	const int taskWorkload = this->totalWorkload / actualNumTasks;
	int leafsRangeBegin = 1;
	for( int i = 0; i < actualNumTasks; ++i ) {
		auto *const task = submittedTasks[i];
		task->leafsRangeBegin = leafsRangeBegin;
		// We have to solve this equation for rangeLength considering leafsRangeBegin and taskWorkload to be known
		// taskWorkload = ( leafsRangeBegin - 1 ) * rangeLength + ( rangeLength * ( rangeLength - 1 ) ) / 2;
		// W = ( B - 1 ) * L + ( L * ( L - 1 ) ) / 2
		// 2 * W = 2 * ( B - 1 ) * L + L * ( L - 1 )
		// 2 * W = 2 * ( B - 1 ) * L + L ^ 2 - L
		// 2 * ( B - 1 ) * L - L + L ^ 2 - 2 * W = 0
		// L ^ 2 + ( 2 * ( B - 1 ) - 1 ) * L - 2 * W = 0
		// assuming d > 0 where
		// d = bCoeff ^ 2 + 4 * 2 * W
		// bCoeff = 2 * ( B - 1 ) - 1
		// roots are: 0.5 * ( -bCoeff +/- d ^ 0.5 )
		const float bCoeff = 2.0f * ( leafsRangeBegin - 1.0f ) - 1.0f;
		float d = bCoeff * bCoeff + 8.0f * taskWorkload;
		assert( d > 0 );
		auto rangeLength = (int)( 0.5f * ( -bCoeff + std::sqrt( d ) ) );
		assert( rangeLength > 0 );
		// See a detailed explanation in task::Exec().
		task->total = ( leafsRangeBegin - 1 ) * rangeLength + ( rangeLength * ( rangeLength - 1 ) ) / 2;
		if( i + 1 != actualNumTasks ) {
			leafsRangeBegin += rangeLength;
			assert( leafsRangeBegin < numLeafs );
			task->leafsRangeEnd = leafsRangeBegin;
			continue;
		}
		// We lose few units due to rounding, so specify the upper bound as it is intended to be.
		task->leafsRangeEnd = numLeafs;
	}

	computationHost->Exec();

#ifndef PUBLIC_BUILD
	ValidateJointResults();
#endif

	return true;
}

template <typename DistanceType>
void PropagationTableBuilder<DistanceType>::ValidateJointResults() {
	const int numLeafs = graphBuilder.NumLeafs();
	if( numLeafs <= 0 ) {
		ValidationError( "Illegal graph NumLeafs() %d", numLeafs );
	}
	const int actualNumLeafs = trap_NumLeafs();
	if( numLeafs != actualNumLeafs ) {
		ValidationError( "graph NumLeafs() %d does not match actual map num leafs %d", numLeafs, actualNumLeafs );
	}

	for( int i = 1; i < numLeafs; ++i ) {
		for( int j = i + 1; j < numLeafs; ++j ) {
			const PropagationProps &iToJ = table[i * numLeafs + j];
			const PropagationProps &jToI = table[j * numLeafs + i];

			if( iToJ.HasDirectPath() ^ jToI.HasDirectPath() ) {
				ValidationError( "Direct path presence does not match for leaves %d, %d", i, j );
			}
			if( iToJ.HasDirectPath() ) {
				if( !std::isfinite( graphBuilder.EdgeDistance( i, j ) ) ) {
					ValidationError( "Graph distance is not finite for leaves %d, %d but props have direct path", i, j );
				}
				continue;
			}

			if( iToJ.HasIndirectPath() ^ jToI.HasIndirectPath() ) {
				ValidationError( "Indirect path presence does not match for leaves %d, %d", i, j );
			}
			if( std::isfinite( graphBuilder.EdgeDistance( i, j ) ) ) {
				ValidationError( "An edge in graph exists for leaves %d, %d but props do not have a direct path", i, j );
			}
			if( !iToJ.HasIndirectPath() ) {
				continue;
			}

			const float pathDistance = iToJ.GetDistance();
			if( !std::isfinite( pathDistance ) || pathDistance <= 0 ) {
				ValidationError( "Illegal propagation distance %f for pair (%d, %d)\n", pathDistance, i, j );
			}

			const auto reversePathDistance = jToI.GetDistance();
			if( reversePathDistance != pathDistance ) {
				const char *format = "Reverse path distance %f does not match direct one %f for leaves %d, %d";
				ValidationError( format, reversePathDistance, pathDistance, i, j );
			}

			// Just check whether these directories are normalized
			// (they are not the same and are not an inversion of each other)
			const char *dirTags[2] = { "direct", "reverse" };
			const PropagationProps *propsRefs[2] = { &iToJ, &jToI };
			for( int k = 0; k < 2; ++k ) {
				vec3_t dir;
				propsRefs[k]->GetDir( dir );
				float length = std::sqrt( VectorLengthSquared( dir ) );
				if( std::abs( length - 1.0f ) > 0.1f ) {
					const char *format = "A dir %f %f %f for %s path between %d, %d is not normalized";
					ValidationError( format, dir[0], dir[1], dir[2], dirTags[k], i, j );
				}
			}
		}
	}
}

template <typename DistanceType>
void PropagationTableBuilder<DistanceType>::ValidationError( const char *format, ... ) {
	char buffer[1024];
	constexpr const char tag[] = "PropagationTableBuilder::ValidateJointResults(): ";
	// Make sure we use the proper size
	static_assert( sizeof( tag ) > sizeof( char * ), "Do not use sizeof( char * ) instead of a real array size" );
	// Copy including the last zero byte
	memcpy( buffer, tag, sizeof( tag ) );
	// Start writing at the zero byte position
	char *writablePtr = buffer + sizeof( tag ) - 1;

	va_list va;
	va_start( va, format );
	Q_vsnprintfz( writablePtr, sizeof( buffer ) -  sizeof( tag ), format, va );
	va_end( va );
	trap_Print( buffer );
	trap_Print("\n");
	abort();
}

template <typename DistanceType>
PropagationBuilderTask<DistanceType>::PropagationBuilderTask( ParentBuilderType *parent_, int numLeafs_ )
	: parent( parent_ ), table( parent->table ), numLeafs( numLeafs_ ), fastAndCoarse( parent->fastAndCoarse ) {
	assert( table && "The table in the parent has not been set" );
}

template <typename DistanceType>
PropagationBuilderTask<DistanceType>::~PropagationBuilderTask() {
	if( graphInstance ) {
		graphInstance->~CloneableGraphBuilder<DistanceType>();
		S_Free( graphInstance );
	}
	if( pathFinderInstance ) {
		pathFinderInstance->~PathFinder<DistanceType>();
		S_Free( pathFinderInstance );
	}
	if( tmpLeafNums ) {
		S_Free( tmpLeafNums );
	}
}


template <typename DistanceType>
void PropagationBuilderTask<DistanceType>::Exec() {
	// Check whether the range has been set and is valid
	assert( leafsRangeBegin > 0 );
	assert( leafsRangeEnd > leafsRangeBegin );
	assert( leafsRangeEnd <= numLeafs );

	// The workload consists of a "rectangle" and a "triangle"
	// The rectangle width (along J - axis) is the range length
	// The rectangle height (along I - axis) is leafsRangeBegin - 1
	// Note that the first row of the table corresponds to a zero leaf and is skipped for processing.
	// Triangle legs have rangeLength size

	// -  -  -  -  -  -  -  -
	// -  *  o  o  o  X  X  X
	// -  o  *  o  o  X  X  X
	// -  o  o  *  o  X  X  X
	// -  o  o  o  *  X  X  X
	// -  o  o  o  o  *  X  X
	// -  o  o  o  o  o  *  X
	// -  o  o  o  o  o  o  *

	assert( this->total > 0 );
	this->executed = 0;
	this->lastReportedProgress = 0;
	this->executedAtLastReport = 0;

	// Process "rectangular" part of the workload
	for( int i = 1; i < leafsRangeBegin; ++i ) {
		for ( int j = leafsRangeBegin; j < leafsRangeEnd; ++j ) {
			ComputePropsForPair( i, j );
		}
	}

	// Process "triangular" part of the workload
	for( int i = leafsRangeBegin; i < leafsRangeEnd; ++i ) {
		for( int j = i + 1; j < leafsRangeEnd; ++j ) {
			ComputePropsForPair( i, j );
		}
	}
}

template <typename DistanceType>
void PropagationBuilderTask<DistanceType>::ComputePropsForPair( int leaf1, int leaf2 ) {
	executed++;
	const auto progress = (int)( 100 * ( executed / (float)total ) );
	// We keep computing progress in percents to avoid confusion
	// but report only even values to reduce threads contention on AddTaskProgress()
	if( progress != lastReportedProgress && !( progress % 2 ) ) {
		int taskWorkloadDelta = executed - executedAtLastReport;
		assert( taskWorkloadDelta > 0 );
		parent->AddTaskProgress( taskWorkloadDelta );
		lastReportedProgress = progress;
		executedAtLastReport = executed;
	}

	PropagationProps *const firstProps = &table[leaf1 * numLeafs + leaf2];
	PropagationProps *const secondProps = &table[leaf2 * numLeafs + leaf1];
	if( graphInstance->EdgeDistance( leaf1, leaf2 ) != std::numeric_limits<double>::infinity() ) {
		firstProps->SetHasDirectPath();
		secondProps->SetHasDirectPath();
		return;
	}

	vec3_t dir1, dir2;
	DistanceType distance;
	if( !BuildPropagationPath( leaf1, leaf2, dir1, dir2, &distance ) ) {
		firstProps->MarkAsFailed();
		secondProps->MarkAsFailed();
		return;
	}

	firstProps->SetIndirectPath( dir1, (float)distance );
	secondProps->SetIndirectPath( dir2, (float)distance );
}

/**
 * A helper for building a weighted sum of normalized vectors.
 */
class WeightedDirBuilder {
public:
	enum : int { MAX_DIRS = 5 };
private:
	vec3_t dirs[MAX_DIRS];
	float weights[MAX_DIRS];
	int numDirs { 0 };
public:
	/**
	 * Reserves a storage for a newly added vector.
	 * @param weight A weight that will be used for a final composition of accumulated data.
	 * @return a writable memory address that must be filled by the added vector.
	 */
	float *AllocDir( double weight ) {
		assert( numDirs < MAX_DIRS );
		assert( !std::isnan( weight ) );
		assert( weight >= 0.0 );
		assert( weight < std::numeric_limits<double>::infinity() );
		weights[numDirs] = (float)weight;
		return dirs[numDirs++];
	}

	/**
	 * Computes a weighted sum of accumulated normalized vectors.
	 * A resulting sum gets normalized.
	 * At least a single vector must be added before this call.
	 * @param dir a storage for a result.
	 */
	void BuildDir( vec3_t dir ) {
		VectorClear( dir );
		assert( numDirs );
		for( int i = 0; i < numDirs; ++i ) {
			VectorMA( dir, weights[i], dirs[i], dir );
		}
		VectorNormalize( dir );
		assert( std::abs( std::sqrt( VectorLengthSquared( dir ) ) - 1.0f ) < 0.1f );
	}
};

template <typename DistanceType>
bool PropagationBuilderTask<DistanceType>::BuildPropagationPath( int leaf1, int leaf2,
																 vec3_t _1to2, vec3_t _2to1,
																 DistanceType *distance ) {
	assert( leaf1 != leaf2 );

	WeightedDirBuilder _1to2Builder;
	WeightedDirBuilder _2to1Builder;

	IteratorType *directIterator;
	IteratorType *reverseIterator;

	// Save a copy of edge weights on demand.
	// Doing that is expensive.
	bool hasModifiedDistanceTable = false;

	double prevPathDistance = 0.0;
	double bestPathDistance = 0.0;
	int numAttempts = 0;
	// Increase quality in developer mode, so we can ship high-quality tables withing the game assets
	// Doing only a single attempt is chosen based on real tests
	// that show that these computations are extremely expensive
	// and could hang up a client computer for a hour (!).
	// Doing a single attempt also helps to avoid saving/restoring weights at all that is not cheap too.
	static_assert( WeightedDirBuilder::MAX_DIRS > 1, "Assumptions that doing only 1 attempt is faster are broken" );
	const int maxAttempts = fastAndCoarse ? 1 : WeightedDirBuilder::MAX_DIRS;
	// Do at most maxAttempts to find an alternative path
	for( ; numAttempts != maxAttempts; ++numAttempts ) {
		DistanceType newPathDistance = pathFinderInstance->FindPath( leaf1, leaf2, &directIterator, &reverseIterator );
		// If the path cannot be (longer) found stop
		if( std::isinf( newPathDistance ) ) {
			break;
		}
		if( !directIterator->HasNext() || !reverseIterator->HasNext() ) {
			break;
		}
		if( !bestPathDistance ) {
			bestPathDistance = newPathDistance;
		}
		// Stop trying to find an alternative path if the new distance is much longer than the previous one
		if( prevPathDistance ) {
			if( newPathDistance > 1.1 * prevPathDistance && newPathDistance - prevPathDistance > 128.0 ) {
				break;
			}
		}

		prevPathDistance = newPathDistance;

		// tmpLeafNums are capacious enough to store slightly more than NumLeafs() * 2 elements
		int *const directLeafNumsEnd = this->tmpLeafNums + graphInstance->NumLeafs() + 1;
		int *directLeafNumsBegin;

		int *const reverseLeafNumsEnd = directLeafNumsEnd + graphInstance->NumLeafs() + 1;
		int *reverseLeafNumsBegin;

		if( numAttempts + 1 != maxAttempts ) {
			if( !hasModifiedDistanceTable ) {
				graphInstance->SaveDistanceTable();
				hasModifiedDistanceTable = true;
			}
			const auto lastDirectLeaf = directIterator->LeafNum();
			const auto firstReverseLeaf = reverseIterator->LeafNum();
			directLeafNumsBegin = UnwindScalingWeights( directIterator, directLeafNumsEnd, 3.0f );
			reverseLeafNumsBegin = UnwindScalingWeights( reverseIterator, reverseLeafNumsEnd, 3.0f );
			graphInstance->ScaleEdgeDistance( lastDirectLeaf, firstReverseLeaf, 3.0f );
		} else {
			directLeafNumsBegin = Unwind( directIterator, directLeafNumsEnd );
			reverseLeafNumsBegin = Unwind( reverseIterator, reverseLeafNumsEnd );
		}

		const double attemptWeight = 1.0f / ( 1.0 + numAttempts );

		assert( *directLeafNumsBegin == leaf1 );
		// Direct leaf nums correspond to the head of the 1->2 path and yield 2->1 "propagation window"
		BuildInfluxDirForLeaf( _2to1Builder.AllocDir( attemptWeight ), directLeafNumsBegin, directLeafNumsEnd );

		assert( *reverseLeafNumsBegin == leaf2 );
		// Reverse leaf nums correspond to the tail of the 1->2 path and yield 1->2 "propagation window"
		BuildInfluxDirForLeaf( _1to2Builder.AllocDir( attemptWeight ), reverseLeafNumsBegin, reverseLeafNumsEnd );
	}

	if( hasModifiedDistanceTable ) {
		graphInstance->RestoreDistanceTable();
	}

	if( !numAttempts ) {
		return false;
	}

	_1to2Builder.BuildDir( _1to2 );
	_2to1Builder.BuildDir( _2to1 );
	*distance = bestPathDistance;
	assert( *distance > 0 && std::isfinite( *distance ) );
	return true;
}

template <typename DistanceType>
int *PropagationBuilderTask<DistanceType>::Unwind( IteratorType *iterator, int *arrayEnd ) {
	int *arrayBegin = arrayEnd;
	// Traverse the direct iterator backwards
	int prevLeafNum = iterator->LeafNum();
	for(;; ) {
		*( --arrayBegin ) = prevLeafNum;
		iterator->Next();
		int nextLeafNum = iterator->LeafNum();
		prevLeafNum = nextLeafNum;
		if( !iterator->HasNext() ) {
			break;
		}
	}
	*( --arrayBegin ) = prevLeafNum;
	return arrayBegin;
}

template <typename DistanceType>
int *PropagationBuilderTask<DistanceType>::UnwindScalingWeights( IteratorType *iterator,
																 int *arrayEnd,
																 DistanceType scale ) {
	int *arrayBegin = arrayEnd;
	// Traverse the direct iterator backwards
	int prevLeafNum = iterator->LeafNum();
	for(;; ) {
		*( --arrayBegin ) = prevLeafNum;
		iterator->Next();
		int nextLeafNum = iterator->LeafNum();

		graphInstance->ScaleEdgeDistance( prevLeafNum, nextLeafNum, scale );

		prevLeafNum = nextLeafNum;
		if( !iterator->HasNext() ) {
			break;
		}
	}
	*( --arrayBegin ) = prevLeafNum;
	return arrayBegin;
}

template <typename DistanceType>
void PropagationBuilderTask<DistanceType>::BuildInfluxDirForLeaf( float *allocatedDir,
																  const int *leafsChain,
																  int numLeafsInChain ) {
	assert( numLeafsInChain > 1 );
	const int maxTestedLeafs = std::min( numLeafsInChain, (int)WeightedDirBuilder::MAX_DIRS );

	WeightedDirBuilder builder;
	for( int i = 1; i < maxTestedLeafs; ++i ) {
		// The graph edge distance might be (temporarily) scaled.
		// However infinity values are preserved.
		if( std::isinf( graphInstance->EdgeDistance( leafsChain[0], leafsChain[i] ) ) ) {
			// If there were added dirs, stop accumulating dirs
			if( i > 1 ) {
				break;
			}

			// Just return a dir from the first leaf to the next leaf without involving the dir builder
			// We do not even check visibility here as we have to provide some valid normalized dir
			// This should not be confusing to a listener as its very likely that secondary emission rays
			// can pass for the most part and can have much greater contribution to an actually used fake source dir.

			// Lets hope this happens rarely enough to avoid caching leaf centers
			vec3_t centers[2];
			for( int j = 0; j < 2; ++j ) {
				const vec3_t *const bounds = trap_GetLeafBounds( leafsChain[i] );
				VectorSubtract( bounds[1], bounds[0], centers[i] );
				VectorScale( centers[i], 0.5f, centers[i] );
				VectorAdd( centers[i], bounds[0], centers[i] );
			}

			VectorSubtract( centers[1], centers[0], allocatedDir );
			VectorNormalize( allocatedDir );
			return;
		}

		// Continue accumulating dirs coming from other leafs to the first one.
		vec3_t dir;
		if( !graphInstance->GetDirFromLeafToLeaf( leafsChain[i], leafsChain[0], dir ) ) {
			assert( 0 && "Should not be reached" );
		}

		// The dir must be present as there is a finite edge distance between leaves

		// We dropped using a distance from leaf to the first leaf
		// as a contribution weight as this distance may be scaled
		// and we do not longer cache leaf centers to compute a raw 3D distance
		float dirWeight = 1.0f - i / (float)maxTestedLeafs;
		dirWeight *= dirWeight;
		// Avoid zero/very small weights diminishing dir contribution
		dirWeight += 0.25f;
		float *const dirToAdd = builder.AllocDir( dirWeight );
		VectorCopy( dir, dirToAdd );
	}

	// Build a result based on all accumulated dirs
	builder.BuildDir( allocatedDir );
}

class PropagationIOHelper {
protected:
	using PropagationProps = PropagationTable::PropagationProps;

	// Try to ensure we can write table elements it as-is regardless of byte order.
	// If there are fields in the bitfield that are greater than a byte and thus
	// require being aware of byte order, the enclosing type cannot (?) be aligned just on byte boundaries.
	static_assert( alignof( PropagationProps ) <= 1, "" );
};

class PropagationTableReader: public CachedComputationReader, protected PropagationIOHelper {
	bool ValidateTable( PropagationProps *propsData, int actualNumLeafs );
public:
	PropagationTableReader( const PropagationTable *parent_, int fsFlags )
		: CachedComputationReader( parent_, fsFlags ) {}

	PropagationProps *ReadPropsTable( int actualNumLeafs );
};

class PropagationTableWriter: public CachedComputationWriter, protected PropagationIOHelper {
public:
	explicit PropagationTableWriter( const PropagationTable *parent_ )
		: CachedComputationWriter( parent_ ) {}

	bool WriteTable( const PropagationTable::PropagationProps *table, int numLeafs );
};

static SingletonHolder<PropagationTable> propagationTableHolder;

PropagationTable *PropagationTable::Instance() {
	return propagationTableHolder.Instance();
}

void PropagationTable::Init() {
	propagationTableHolder.Init();
}

void PropagationTable::Shutdown() {
	propagationTableHolder.Shutdown();
}

inline void PropagationTable::PropagationProps::SetDir( const vec3_t dir ) {
	int byte = DirToByteTable::DirToByte( dir );
	assert( (unsigned)byte < std::numeric_limits<uint8_t>::max() - 1 );
	maybeDirByte = (uint8_t)byte;
}

inline void PropagationTable::PropagationProps::SetIndirectPath( const vec3_t dir, float distance ) {
	SetDistance( distance );
	SetDir( dir );
}

bool PropagationTable::TryReadFromFile( int fsFlags ) {
	PropagationTableReader reader( this, fsFlags );
	return ( this->table = reader.ReadPropsTable( NumLeafs() ) ) != nullptr;
}

bool PropagationTable::ComputeNewState( bool fastAndCoarse ) {
	if( fastAndCoarse ) {
		PropagationTableBuilder<float> builder( NumLeafs(), fastAndCoarse );
		if( builder.Build()) {
			table = builder.ReleaseOwnership();
			return true;
		}
		return false;
	}

	PropagationTableBuilder<double> builder( NumLeafs(), fastAndCoarse );
	if( builder.Build()) {
		table = builder.ReleaseOwnership();
		return true;
	}

	return false;
}

void PropagationTable::ProvideDummyData() {
	size_t memSize = sizeof( PropagationProps ) * NumLeafs() * NumLeafs();
	table = (PropagationProps *)S_Malloc( memSize );
	memset( table, 0, memSize );
}

bool PropagationTable::SaveToCache() {
	if( !NumLeafs() ) {
		return true;
	}

	PropagationTableWriter writer( this );
	return writer.WriteTable( this->table, NumLeafs() );
}

bool PropagationTableReader::ValidateTable( PropagationIOHelper::PropagationProps *propsData, int actualNumLeafs ) {
	const int maxByteValue = std::numeric_limits<uint8_t>::max();
	for( int i = 0, end = actualNumLeafs * actualNumLeafs; i < end; ++i ) {
		int dirByte = propsData[i].maybeDirByte;
		if( !DirToByteTable::IsValidDirByte( dirByte ) ) {
			if( dirByte != maxByteValue && dirByte != maxByteValue - 1 ) {
				return false;
			}
		}
	}
	return true;
}

PropagationTableReader::PropagationProps *PropagationTableReader::ReadPropsTable( int actualNumLeafs ) {
	// Sanity check
	assert( actualNumLeafs > 0 && actualNumLeafs < ( 1 << 20 ) );

	if( fsResult < 0 ) {
		return nullptr;
	}

	int32_t savedNumLeafs;
	if( !ReadInt32( &savedNumLeafs ) ) {
		fsResult = -1;
		return nullptr;
	}

	if( savedNumLeafs != actualNumLeafs ) {
		fsResult = -1;
		return nullptr;
	}

	size_t expectedSize = actualNumLeafs * actualNumLeafs * sizeof( PropagationProps );
	// TODO:... this is pretty bad..
	// Just return a view of the file data that is read and is kept in-memory.
	// An overhead of storing few extra strings at the beginning is insignificant.
	// Never returns on failure?
	auto *const result = (PropagationProps *)S_Malloc( expectedSize );
	if( Read( result, expectedSize ) ) {
		if( ValidateTable( result, actualNumLeafs ) ) {
			return result;
		}
	}

	S_Free( result );
	fsResult = -1;
	return nullptr;
}

bool PropagationTableWriter::WriteTable( const PropagationTable::PropagationProps *table, int numLeafs ) {
	// Sanity check
	assert( numLeafs > 0 && numLeafs < ( 1 << 20 ) );

	if( fsResult < 0 ) {
		return false;
	}

	if( !WriteInt32( numLeafs ) ) {
		return false;
	}

	return Write( table, numLeafs * numLeafs * sizeof( PropagationProps ) );
}

class CachedGraphReader: public CachedComputationReader {
public:
	CachedGraphReader( const CachedLeafsGraph *parent_, int fsFlags )
		: CachedComputationReader( parent_, fsFlags ) {}

	bool Read( CachedLeafsGraph *readObject );
};

class CachedGraphWriter: public CachedComputationWriter {
public:
	explicit CachedGraphWriter( const CachedLeafsGraph *parent_ )
		: CachedComputationWriter( parent_ ) {}

	bool Write( const CachedLeafsGraph *writtenObject );
};

static SingletonHolder<CachedLeafsGraph> leafsGraphHolder;

CachedLeafsGraph *CachedLeafsGraph::Instance() {
	return leafsGraphHolder.Instance();
}

void CachedLeafsGraph::Init() {
	leafsGraphHolder.Init();
}

void CachedLeafsGraph::Shutdown() {
	leafsGraphHolder.Shutdown();
}

CachedLeafsGraph::~CachedLeafsGraph() {
	// Can't be defined in header due to this call
	TaggedAllocators::FreeUsingMetadata( dirsTable, "dirs table of global graph" );
}

void CachedLeafsGraph::ResetExistingState() {
	distanceTable = TaggedAllocators::FreeUsingMetadata( distanceTable, "distance table of global graph" );
	dirsTable = TaggedAllocators::FreeUsingMetadata( dirsTable, "dirs table of global graph" );
	adjacencyListsData = TaggedAllocators::FreeUsingMetadata( adjacencyListsData, "lists data of global graph" );
	adjacencyListsData = nullptr;
	// Just nullify the pointer. A corresponding chunk belongs to the lists data.
	adjacencyListsOffsets = nullptr;
}

bool CachedLeafsGraph::TryReadFromFile( int fsFlags ) {
	CachedGraphReader reader( this, fsFlags );
	return reader.Read( this );
}

bool CachedLeafsGraph::ComputeNewState( bool fastAndCoarse_ ) {
	const int actualNumLeafs = CachedComputation::NumLeafs();
	// Always set the number of leafs for the graph even if we have not managed to build the graph.
	// The number of leafs in the CachedComputation will be always set by its EnsureValid() logic.
	// Hack... we have to resolve multiple inheritance ambiguity.
	( ( ParentGraphType *)this)->numLeafs = actualNumLeafs;

	PropagationGraphBuilder<float> builder( actualNumLeafs, fastAndCoarse_ );
	// Specify "this" as a target to suppress an infinite recursion while trying to reuse the global graph
	if( !builder.Build( this ) ) {
		return false;
	}

	// The builder should no longer own the distance table and the leafs lists data.
	// They should be freed using TaggedAllocator::FreeUsingMetadata() on our own.
	builder.TransferOwnership( &this->distanceTable,
							   &this->dirsTable,
							   &this->adjacencyListsData,
							   &this->adjacencyListsOffsets );

	// TODO: Transfer the data size explicitly instead of relying on implied data offset
	this->leafListsDataSize = (int)( this->adjacencyListsOffsets - this->adjacencyListsData );
	this->leafListsDataSize += actualNumLeafs;

	return true;
}

void CachedLeafsGraph::ProvideDummyData() {
	// Allocate a small chunk for the table... it is not going to be accessed
	// That's bad...
	this->distanceTable = TaggedAllocatorForType<float>().Alloc( 1, "(dummy) distance table" );
	// Allocate a dummy cell for a dummy list and a full row for offsets
	auto *leafsData = TaggedAllocatorForType<int>().Alloc( NumLeafs() + 1, "(dummy) adjacency data" );
	// Put the dummy list at the beginning
	leafsData[0] = 0;
	// Make all offsets refer to the dummy list
	memset( leafsData + 1, 0, sizeof( int ) * NumLeafs() );
	this->adjacencyListsData = leafsData;
	this->adjacencyListsOffsets = leafsData + 1;
	// Lets try avoiding allocating dirs table since its huge
}

bool CachedLeafsGraph::SaveToCache() {
	CachedGraphWriter writer( this );
	return writer.Write( this );
}

struct SoundMemDeleter {
	void operator()( void *p ) {
		if( p ) {
			S_Free( p );
		}
	}
};

template <typename T>
struct TaggedAllocatorCaller {
	void operator()( T *p ) {
		TaggedAllocators::FreeUsingMetadata( p, "Generic TaggedAllocator caller" );
	}
};


bool CachedGraphReader::Read( CachedLeafsGraph *readObject ) {
	if( fsResult < 0 ) {
		return false;
	}

	int32_t numLeafs;
	if( !ReadInt32( &numLeafs ) ) {
		return false;
	}

	// Sanity check
	if( numLeafs < 1 || numLeafs > ( 1 << 24 ) ) {
		return false;
	}

	// Read the lists data size. Note that it is specified in int elements and not in bytes.
	int32_t listsDataSize;
	if( !ReadInt32( &listsDataSize ) ) {
		return false;
	}

	// Sanity check
	if( listsDataSize < numLeafs ) {
		return false;
	}

	const size_t numBytesForDistanceTable = numLeafs * numLeafs * sizeof( float );
	// Dummy rows/columns for zero leaf are not stored.
	// Only the upper triangle above the table diagonal is stored.
	const int numStoredDirs = ( ( numLeafs - 1 ) * ( numLeafs - 1 ) ) - ( numLeafs - 1 );
	const size_t numBytesForDirsTable = sizeof( uint8_t ) * numStoredDirs;
	const size_t numBytesForLists = listsDataSize * sizeof( int );
	if( BytesLeft() != numBytesForDistanceTable + numBytesForDirsTable + numBytesForLists ) {
		return false;
	}

	using DistanceTableHolder = std::unique_ptr<float, TaggedAllocatorCaller<float>>;
	float *distanceData = ::RefCountingAllocatorForType<float>()
	    .Alloc( numLeafs * numLeafs, "graph distance table of global graph" );

	DistanceTableHolder distanceTableHolder( distanceData );
	if( !CachedComputationReader::Read( distanceTableHolder.get(), numBytesForDistanceTable ) ) {
		return false;
	}

	using DirsTableHolder = std::unique_ptr<uint8_t, TaggedAllocatorCaller<uint8_t>>;
	uint8_t *dirsData = ::RefCountingAllocatorForType<uint8_t>()
	    .Alloc( numLeafs * numLeafs, "graph dirs data of global graph" );

	DirsTableHolder	dirsTableHolder( dirsData );
	if( !CachedComputationReader::Read( dirsTableHolder.get(), numBytesForDirsTable ) ) {
		return false;
	}

	// Validate dirs
	for( int i = 0; i < numStoredDirs; ++i ) {
		if( !DirToByteTable::IsValidDirByte( dirsData[i] ) ) {
			if( dirsData[i] != std::numeric_limits<uint8_t>::max() ) {
				return false;
			}
		}
	}

	using ListsDataHolder = std::unique_ptr<int, TaggedAllocatorCaller<int>>;
	int *listsData = TaggedAllocatorForType<int>().Alloc( listsDataSize, "adjacency data of global graph" );
	ListsDataHolder listsDataHolder( listsData );
	if( !CachedComputationReader::Read( listsDataHolder.get(), numBytesForLists ) ) {
		return false;
	}

	// Swap bytes in lists and validate lists first
	// There is some chance of a failure.
	const int *const listsDataBegin = listsDataHolder.get();
	int *const offsets = listsDataHolder.get() + listsDataSize - numLeafs;
	const int *const listsDataEnd = listsDataHolder.get() + listsDataSize - numLeafs;
	if( offsets <= listsDataBegin ) {
		return false;
	}

	// Byte-swap and validate offsets
	int prevOffset = 0;
	for( int i = 1; i < numLeafs; ++i ) {
		offsets[i] = LittleLong( offsets[i] );
		if( offsets[i] < 0 ) {
			return false;
		}
		if( offsets[i] >= listsDataSize - numLeafs ) {
			return false;
		}
		if( offsets[i] <= prevOffset ) {
			return false;
		}
		prevOffset = offsets[i];
	}

	// Byte-swap and validate lists
	int tableRowOffset = 0;

	// Start from the list for element #1
	// Retrieval data for a zero leaf is illegal anyway.
	const int *expectedNextListAddress = listsDataHolder.get() + 1;
	for( int i = 1; i < numLeafs; ++i ) {
		tableRowOffset += i * numLeafs;
		// We have ensured this offset is valid
		int *list = listsDataHolder.get() + offsets[i];
		// Check whether the list follows the previous list
		if( list != expectedNextListAddress ) {
			return false;
		}
		// Swap bytes in the memory, copy to a local variable only after that!
		*list = LittleLong( *list );
		// The first element of the list is it's size. Check it for sanity first.
		const int listSize = *list++;
		if( listSize < 0 || listSize > ( 1 << 24 ) ) {
			return false;
		}
		// Check whether accessing elements in range defined by this size is allowed.
		if( list + listSize > listsDataEnd ) {
			return false;
		}
		for( int j = 0; j < listSize; ++j ) {
			list[j] = LittleLong( list[j] );
			// Check whether its a valid non-zero leaf
			if( list[j] < 1 || list[j] >= numLeafs ) {
				return false;
			}
		}
		expectedNextListAddress = list + listSize;
	}

	for( int i = 0, end = numLeafs * numLeafs; i < end; ++i ) {
		distanceTableHolder.get()[i] = LittleLong( distanceTableHolder.get()[i] );
	}

	readObject->distanceTable = distanceTableHolder.release();
	readObject->dirsTable = dirsTableHolder.release();
	readObject->adjacencyListsData = listsDataHolder.release();
	readObject->adjacencyListsOffsets = readObject->adjacencyListsData + listsDataSize - numLeafs;
	readObject->leafListsDataSize = listsDataSize;
	( (CachedLeafsGraph::ParentGraphType *)readObject )->numLeafs = numLeafs;
	return true;
}

bool CachedGraphWriter::Write( const CachedLeafsGraph *writtenObject ) {
	static_assert( sizeof( int32_t ) == sizeof( int ), "" );

	const int numLeafs = writtenObject->NumLeafs();
	if( !WriteInt32( numLeafs ) ) {
		return false;
	}

	auto listsDataSize = (int)( writtenObject->adjacencyListsOffsets - writtenObject->adjacencyListsData );
	assert( listsDataSize > 0 );
	// Add offsets data size (which is equal to number of lists) to the total lists data size.
	// Note: the data size is assumed to be in integer elements and not in bytes.
	// The reader expects the total data size and expects offsets at the end of this data minus the number of lists.
	listsDataSize += numLeafs;

	if( !WriteInt32( listsDataSize ) ) {
		return false;
	}
	if( !CachedComputationWriter::Write( writtenObject->distanceTable, numLeafs * numLeafs * sizeof( float ) ) ) {
		return false;
	}

	const int numStoredDirs = ( ( numLeafs - 1 ) * ( numLeafs - 1 ) ) - ( numLeafs - 1 );
	const size_t dirsDataSize = sizeof( uint8_t ) * numStoredDirs;
	if( !CachedComputationWriter::Write( writtenObject->dirsTable, dirsDataSize ) ) {
		return false;
	}

	return CachedComputationWriter::Write( writtenObject->adjacencyListsData, listsDataSize * sizeof( int ) );
}

template <typename AdjacencyListType, typename DistanceType>
bool GraphBuilder<AdjacencyListType, DistanceType>::TryUsingGlobalGraph( TargetType *target ) {
	const auto globalGraph = CachedLeafsGraph::Instance();
	// Can't be used for the global graph itself (falls into an infinite recursion)
	// WARNING! We have to force the desired type of the object first to avoid comparison of different pointers,
	// then erase the type to make it compiling. `this` differs in context of different base classes of an object.
	if( ( void *)static_cast<GraphLike<int, float> *>( globalGraph ) == (void *)target ) {
		return false;
	}

	globalGraph->EnsureValid();
	if( globalGraph->IsUsingValidData() ) {
		return false;
	}

	const int numLeafs = globalGraph->NumLeafs();
	const int listsDataSize = globalGraph->LeafListsDataSize();

	TaggedAllocator<AdjacencyListType> *listsDataAllocator;
	TaggedAllocator<DistanceType> *tableScratchpadAllocator = nullptr;
	if( auto *thatBuilderLike = dynamic_cast<GraphBuilder<AdjacencyListType, DistanceType> *>( target ) ) {
		tableScratchpadAllocator = &thatBuilderLike->TableScratchpadAllocator();
		listsDataAllocator = &thatBuilderLike->AdjacencyListsAllocator();
	} else {
		// Initialize using a ref-counting allocator anyway.
		listsDataAllocator = &RefCountingAllocatorForType<AdjacencyListType>();
	}

	this->adjacencyListsData = listsDataAllocator->Alloc( listsDataSize, "clone lists data" );
	this->adjacencyListsOffsets = this->adjacencyListsData + listsDataSize - numLeafs;

	// If the global graph has been built/loaded successfully
	// its lists must be valid, so assertions are put.
	// However, data loss due to using lesser types is treated as inability to reuse the global graph.
	// TODO: Does it mean the graph cannot be built for the types pair of this builder?

	// The first element of the global graph is a dummy value for zero list
	// (so we do not have to correct a list index every time we perform an access)
	// However thats where lists data actually begin.
	const int *const thatListsDataBegin = globalGraph->AdjacencyList( 1 ) - 1;
	AdjacencyListType *thisData = this->adjacencyListsData;
	// Put a dummy value for the zero leaf
	*thisData++ = 0;
	for( int i = 1; i < numLeafs; ++i ) {
		const int *thatList = globalGraph->AdjacencyList( i );
		const ptrdiff_t actualOffset = thatList - thatListsDataBegin;
		// Must be valid
		assert( actualOffset >= 0 );
		// Check for overflow if casting to a lesser type
		if( actualOffset > std::numeric_limits<DistanceType>::max() ) {
			return false;
		}
		this->adjacencyListsOffsets[i] = (AdjacencyListType)( actualOffset );
		const int listSize = *thatList++;
		// Check the length for sanity
		assert( listSize >= 0 );
		// Add the list length to the lists data of this builder
		*thisData++ = (AdjacencyListType)listSize;
		for( int j = 0; j < listSize; ++j ) {
			const int leafNum = thatList[j];
			// Check whether the leaf num is valid
			assert( leafNum > 0 && leafNum < numLeafs );
			// Check for overflow if casting to a lesser type
			if( leafNum > std::numeric_limits<DistanceType>::max() ) {
				return false;
			}
			// Add the list element to the lists data of this builder
			*thisData++ = (AdjacencyListType)leafNum;
		}
	}

	// Must match the beginning of the offsets after lists data has been written
	assert( thisData == this->adjacencyListsOffsets );

	if( auto *mutableGraphTarget = dynamic_cast<MutableGraph<AdjacencyListType, DistanceType> *>( target ) ) {
		if( !tableScratchpadAllocator ) {
			// Let's use a basic tagged allocator (that does not allow sharing)
			// so we can spot attempts of sharing mutable per-instance data easily
			tableScratchpadAllocator = &TaggedAllocatorForType<DistanceType>();
		}
		mutableGraphTarget->distanceTableScratchpad =
			tableScratchpadAllocator->Alloc( numLeafs * numLeafs, "scratchpad of a clone" );
	}

	this->distanceTable = this->distanceTableBackup = ReuseGlobalDistanceTable<DistanceType>( numLeafs );

	return true;
}

template <typename DistanceType>
CloneableGraphBuilder<DistanceType> *CloneableGraphBuilder<DistanceType>::Clone() {
	// TODO: Use just malloc() and check results? A caller code must be aware of possible failure
	void *objectMem = S_Malloc( sizeof( CloneableGraphBuilder<DistanceType> ) );
	if( !objectMem ) {
		return nullptr;
	}

	using CloneHolder = std::unique_ptr<CloneableGraphBuilder<DistanceType>, SoundMemDeleter>;
	CloneHolder clone( new( objectMem )CloneableGraphBuilder<DistanceType>( this->NumLeafs(), this->fastAndCoarse ) );

	clone->distanceTable = clone->distanceTableBackup =
		this->TableBackupAllocator().AddRef( this->distanceTableBackup, "cloned table backup" );

	clone->distanceTableScratchpad =
		clone->TableScratchpadAllocator().Alloc( this->NumLeafs() * this->NumLeafs(), "scratchpad of a clone" );

	if( !clone->distanceTableScratchpad ) {
		return nullptr;
	}

	clone->adjacencyListsData = this->AdjacencyListsAllocator().AddRef( this->adjacencyListsData, "lists data of a clone" );
	// Just copy the address... offsets are allocated within the lists data memory chunk
	clone->adjacencyListsOffsets = this->adjacencyListsOffsets;

	clone->dirsTable = this->DirsAllocator().AddRef( this->dirsTable, "dirs of a clone" );

	return clone.release();
}