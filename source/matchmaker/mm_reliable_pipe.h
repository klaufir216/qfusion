#ifndef QFUSION_MM_REPORTS_UPLOADER_H
#define QFUSION_MM_REPORTS_UPLOADER_H

#include "mm_reports_storage.h"

class ReliablePipe {
	template <typename T> friend class SingletonHolder;

	/**
	 * A common supertype for things that run in a background thread
	 * sleeping and checking their state periodically
	 * doing that until signaled for termination.
	 */
	class BackgroundRunner {
		friend class ReliablePipe;
	protected:
		std::atomic<bool> signaledForTermination { false };
		const char *const logTag;
		LocalReportsStorage *const reportsStorage;

		BackgroundRunner( const char *logTag_, LocalReportsStorage *reportsStorage_ )
			: logTag( logTag_ ), reportsStorage( reportsStorage_ ) {}

		virtual ~BackgroundRunner() = default;

		virtual bool CanTerminate() const {
			return signaledForTermination.load( std::memory_order_relaxed );
		}

		void RunMessageLoop();

		virtual void RunStep() = 0;

		static void *ThreadFunc( void *param );

		void SignalForTermination() {
			signaledForTermination.store( true, std::memory_order_relaxed );
		}

		virtual void DropReport( QueryObject *report );
	};

	/**
	 * A {@code BackgroundRunner} that listens for match reports
	 * delivered via a buffered pipe and tries to store reports in a transaction.
	 */
	class BackgroundWriter final : public BackgroundRunner {
		struct qbufPipe_s *const pipe;
		/**
		 * A report we currently try writing to the database.
		 * @note do not confuse with {@code BackgroundSender::activeReport}.
		 */
		QueryObject *activeReport { nullptr };

		unsigned numRetries { 0 };
	public:
		BackgroundWriter( LocalReportsStorage *reportsStorage_, struct qbufPipe_s *pipe_ )
			: BackgroundRunner( "BackgroundWriter", reportsStorage_ ), pipe( pipe_ ) {}

		struct AddReportCmd {
			int id;
			BackgroundWriter *self;
			QueryObject *report;

			AddReportCmd() = default;
			AddReportCmd( BackgroundWriter *self_, QueryObject *report_ ): id( 0 ), self( self_ ), report( report_ ) {}
		};

		void AddReport( QueryObject *report ) {
			// The pipe must be read only if there's no active report
			assert( !activeReport );
			activeReport = report;
			numRetries = 0;
		}

		void DeleteActiveReport() {
			assert( activeReport );
			QueryObject::DeleteQuery( activeReport );
			activeReport = nullptr;
			numRetries = 0;
		}

		void RunStep() override;

		typedef unsigned ( *Handler )( const void * );
		static Handler pipeHandlers[1];

		static unsigned AddReportHandler( const void * );
	};

	/**
	 * A {@code BackgroundRunner} that wraps in a transaction
	 * reading non-sent reports from a storage, sending reports
	 * over network and marking report delivery status in the storage.
	 */
	class BackgroundSender final : public BackgroundRunner {
		/**
		 * A report we try to fill using form name-value pairs stored in database.
		 * @note do not confuse with {@code BackgroundWriter::activeReport}.
		 */
		QueryObject *activeReport { nullptr };
	public:
		explicit BackgroundSender( LocalReportsStorage *reportsStorage_ )
			: BackgroundRunner( "BackgroundSender", reportsStorage_ ) {}

		~BackgroundSender() override {
			if( activeReport ) {
				QueryObject::DeleteQuery( activeReport );
			}
		}

		void RunStep() override;

		void DeleteActiveReport() {
			assert( activeReport );
			QueryObject::DeleteQuery( activeReport );
			activeReport = nullptr;
		}
	};

	LocalReportsStorage reportsStorage;

	BackgroundWriter *backgroundWriter { nullptr };
	BackgroundSender *backgroundSender { nullptr };

	struct qbufPipe_s *reportsPipe { nullptr };
	struct qthread_s *writerThread { nullptr };
	struct qthread_s *senderThread { nullptr };

	static const char *MakeLocalStoragePath();

	ReliablePipe();
	~ReliablePipe();
public:
	static void Init();
	static void Shutdown();
	static ReliablePipe *Instance();

	void EnqueueMatchReport( QueryObject *matchReport );
};

#endif
