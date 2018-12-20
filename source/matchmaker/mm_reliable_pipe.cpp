#include "mm_reliable_pipe.h"

#include "../qalgo/SingletonHolder.h"

#include "../qcommon/qthreads.h"
#include "../qcommon/qcommon.h"

static SingletonHolder<ReliablePipe> uploaderInstanceHolder;

void ReliablePipe::Init() {
	uploaderInstanceHolder.Init();
}

void ReliablePipe::Shutdown() {
	uploaderInstanceHolder.Shutdown();
}

ReliablePipe *ReliablePipe::Instance() {
	return uploaderInstanceHolder.Instance();
}

const char *ReliablePipe::MakeLocalStoragePath() {
	return va( "%s/mm_db_for_port_%d", FS_CacheDirectory(), (unsigned)Cvar_Value( "sv_port" ) );
}

void *ReliablePipe::BackgroundRunner::ThreadFunc( void *param ) {
	// Wait for launching runner loops more for a listen server.
	// We do not want losing messages below in video/other console spam.
	Sys_Sleep( ( Cvar_Value( "dedicated" ) != 0.0f ) ? 3 * 1000 : 15 * 1000 );

	auto *const runner = (BackgroundRunner *)param;
	Com_Printf( "Launching matchmaker/stats ReliablePipe::%s...\n", runner->logTag );
	runner->RunMessageLoop();
	Com_Printf( "Stopping matchmaker/stats ReliablePipe::%s...\n", runner->logTag );
	return nullptr;
}

void ReliablePipe::BackgroundRunner::RunMessageLoop() {
	while( !CanTerminate() ) {
		RunStep();
	}
}

ReliablePipe::ReliablePipe()
	: reportsStorage( MakeLocalStoragePath() ) {
	// Never actually fails?
	this->reportsPipe = QBufPipe_Create( 128, 1 );

	// Never actually fails?
	this->backgroundWriter = new( ::malloc( sizeof( BackgroundWriter ) ) )BackgroundWriter( &reportsStorage, reportsPipe );
	this->backgroundSender = new( ::malloc( sizeof( BackgroundSender ) ) )BackgroundSender( &reportsStorage );

	// Never returns on fail?
	this->writerThread = QThread_Create( &BackgroundWriter::ThreadFunc, backgroundWriter );
	this->senderThread = QThread_Create( &BackgroundRunner::ThreadFunc, backgroundSender );
}

ReliablePipe::~ReliablePipe() {
	backgroundWriter->SignalForTermination();
	backgroundSender->SignalForTermination();
	QThread_Join( writerThread );
	QThread_Join( senderThread );

	backgroundWriter->~BackgroundWriter();
	backgroundSender->~BackgroundSender();

	QBufPipe_Destroy( &reportsPipe );
}

void ReliablePipe::EnqueueMatchReport( QueryObject *matchReport ) {
	BackgroundWriter::AddReportCmd cmd( backgroundWriter, matchReport );
	QBufPipe_WriteCmd( reportsPipe, &cmd, sizeof( cmd ) );
}

ReliablePipe::BackgroundWriter::Handler ReliablePipe::BackgroundWriter::pipeHandlers[1] = {
	&ReliablePipe::BackgroundWriter::AddReportHandler
};

void ReliablePipe::BackgroundRunner::DropReport( QueryObject *report ) {
	// TODO: What to do?
	// This only can occur if the database keeps being locked.
	// Writing reports to a file system can preserve report data but we dislike this approach.
	// TODO: Maybe link in a queue and try writing while idle?
	Com_Printf( S_COLOR_RED "Warning: Can't write a match report to a database, dropping the report..." );
}

unsigned ReliablePipe::BackgroundWriter::AddReportHandler( const void *data ) {
	AddReportCmd cmd;
	memcpy( &cmd, data, sizeof( AddReportCmd ) );
	cmd.self->AddReport( cmd.report );
	return (unsigned)sizeof( AddReportCmd );
}

void ReliablePipe::BackgroundWriter::RunStep() {
	Sys_Sleep( 32 );
	// If there is no active report
	if( !activeReport ) {
		// Check the pipe for ingoing reports
		QBufPipe_ReadCmds( pipe, pipeHandlers );
		// If there is still no active report
		if( !activeReport ) {
			return;
		}
	}

	assert( activeReport );

	bool hasInsertionSucceeded = false;
	bool hasTransactionSucceeded = reportsStorage->WithinTransaction([&]( DbConnection connection ) {
		hasInsertionSucceeded = reportsStorage->Push( connection, activeReport );
		// Returning true means the transaction should be committed
		return true;
	});

	if( hasTransactionSucceeded ) {
		if( !hasInsertionSucceeded ) {
			DropReport( activeReport );
		}
		DeleteActiveReport();
		return;
	}

	// Looks like the database is locked by sender.
	// Try inserting the active report again next step.

	if( numRetries < 5 ) {
		numRetries++;
		return;
	}

	DropReport( activeReport );
	DeleteActiveReport();
}

void ReliablePipe::BackgroundSender::RunStep() {
	Sys_Sleep( 16 );

	// If there is no dummy report to fill, create it
	if( !activeReport ) {
		// TODO: Do URL construction once at singleton construction?
		char url[MAX_QPATH];
		Q_snprintfz( url, sizeof( url ), "%s/server/matchReport", Cvar_String( "mm_url" ) );
		activeReport = QueryObject::NewPostQuery( url, Cvar_String( "sv_ip" ) );
	}

	unsigned sleepInterval = 667;
	reportsStorage->WithinTransaction([&]( DbConnection connection ) {
		if( !( reportsStorage->FetchNextReport( connection, activeReport ) ) ) {
			// No active report is present in the database yet.
			// Writer threads have plenty of time for performing their transactions in this case
			sleepInterval = 1500;
			// Returning true means the transaction should be committed
			return true;
		}

		activeReport->SendForStatusPolling();
		while( !activeReport->IsReady() ) {
			Sys_Sleep( 16 );
			QueryObject::Poll();
		}

		if( activeReport->HasSucceeded() ) {
			sleepInterval = 1500;
			// Request committing or rolling back depending of result of this call
			bool result = reportsStorage->MarkReportAsSent( connection, activeReport );
			DeleteActiveReport();
			return result;
		}

		// This is more useful for non-persistent/reliable queries like client login.
		// In this scenario let's retry implicitly on next RunStep() call.
		// Fetching the same report (with the same id) is not guaranteed
		// but we should not rely on reports ordering.
		// This is to give a breathe for writer thread that is possibly tries
		// to open a transaction while we're still holding a database exclusive lock.
		if( activeReport->ShouldRetry() ) {
			DeleteActiveReport();
			return true;
		}

		assert( activeReport->IsReady() && !activeReport->HasSucceeded() && !activeReport->ShouldRetry() );

		// Request committing or rolling back depending of result of this call
		bool result = reportsStorage->MarkReportAsFailed( connection, activeReport );
		DeleteActiveReport();
		return result;
	});

	Sys_Sleep( sleepInterval );
}
