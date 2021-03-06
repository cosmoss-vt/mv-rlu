/**
 * @author Aleksandar Dragojevic aleksandar.dragojevic@epfl.ch
 *
 */

#ifndef WLPDSTM_DYNAMIC_LAZY_IMPL_INLINE_H_
#define WLPDSTM_DYNAMIC_LAZY_IMPL_INLINE_H_

inline wlpdstm::TxLazy::TryCommitResult wlpdstm::TxLazy::TxTryCommit(TransactionDynamic *desc) {
	Word ts = desc->valid_ts;
	
	if(!LockWriteSet(desc)) {
		return RESTART_RUNNING;
	}
	
	// now get a commit timestamp
	ts = desc->IncrementCommitTs();
	
	// if global time overflows restart
	if(ts >= MAXIMUM_TS) {
		//executing = false;
		desc->tx_status = (Word)TX_ABORTED;
#ifdef PRIVATIZATION_QUIESCENCE
		*desc->quiescence_ts = MINIMUM_TS;
#endif /* PRIVATIZATION_QUIESCENCE */
		// this is a special case where no jump is required
		RollbackCommitting(desc);
		
		if(desc->StartSynchronization()) {
			desc->RestartCommitTS();
			desc->EndSynchronization();
			desc->stats.IncrementStatistics(StatisticsType::CLOCK_OVERFLOWS);
		}
		
		return JUMP_RESTART;
	}
	
	// if there is no validation in GV4, then the read set of one transaction could
	// overlap with the write set of another and this would pass unnoticed
#ifdef COMMIT_TS_INC
	if(ts != desc->valid_ts + 1 && !desc->ValidateWithReadLockVersions()) {
#elif defined COMMIT_TS_GV4
	if(!desc->ValidateWithReadLockVersions()) {
#endif /* commit_ts */
		desc->stats.IncrementStatistics(StatisticsType::ABORT_COMMIT_VALIDATE);
		return RESTART_COMMITTING;
	}
	
	VersionLock commitVersion = get_version_lock(ts);
	
	// now update all written values
	for(WriteLog::iterator iter = desc->write_log.begin();iter.hasNext();iter.next()) {
		WriteLogEntry &entry = *iter;
		
		// now update actual values
		WriteWordLogEntry *word_log_entry = entry.head;
		
		while(word_log_entry != NULL) {
			*word_log_entry->address = word_log_entry->MaskWord();
			word_log_entry = word_log_entry->next;
		}
		
		// release locks
		atomic_store_release(entry.read_lock, commitVersion);
		atomic_store_release(entry.write_lock, WRITE_LOCK_CLEAR);
	}
	
	atomic_store_release(&desc->tx_status, TX_COMMITTED);
	
#ifdef PRIVATIZATION_QUIESCENCE
	atomic_store_release(desc->quiescence_ts, MINIMUM_TS);
	desc->PrivatizationQuiescenceWait(ts);
#endif /* PRIVATIZATION_QUIESCENCE */
	
	desc->write_log.clear();
	desc->write_log_hashtable.clear();
	desc->read_log.clear();
	
	// commit mem
	desc->mm.TxCommit<TransactionDynamic>(ts);
	
	desc->stats.IncrementStatistics(StatisticsType::COMMIT);
	desc->succ_aborts = 0;
	
	return COMMIT;				
	
}

inline wlpdstm::TxLazy::TryCommitResult wlpdstm::TxLazy::TxTryCommitStatic(TransactionDynamic *desc) {
	Word ts = desc->valid_ts;
	bool read_only = desc->write_log.empty();
	
	if(!read_only) {
		if(!LockWriteSet(desc)) {
			return RESTART_RUNNING;
		}
		
		// now get a commit timestamp
		ts = desc->IncrementCommitTs();
		
		// if global time overflows restart
		if(ts >= MAXIMUM_TS) {
			//executing = false;
			desc->tx_status = (Word)TX_ABORTED;
#ifdef PRIVATIZATION_QUIESCENCE
			*desc->quiescence_ts = MINIMUM_TS;
#endif /* PRIVATIZATION_QUIESCENCE */
			// this is a special case where no jump is required
			RollbackCommitting(desc);
			
			if(desc->StartSynchronization()) {
				desc->RestartCommitTS();
				desc->EndSynchronization();
				desc->stats.IncrementStatistics(StatisticsType::CLOCK_OVERFLOWS);
			}
			
			return JUMP_RESTART;
		}
		
		// if there is no validation in GV4, then the read set of one transaction could
		// overlap with the write set of another and this would pass unnoticed
#ifdef COMMIT_TS_INC
		if(ts != desc->valid_ts + 1 && !desc->ValidateWithReadLockVersions()) {
#elif defined COMMIT_TS_GV4
		if(!desc->ValidateWithReadLockVersions()) {
#endif /* commit_ts */
			desc->stats.IncrementStatistics(StatisticsType::ABORT_COMMIT_VALIDATE);
			return RESTART_COMMITTING;
		}
		
		VersionLock commitVersion = get_version_lock(ts);
		
		// now update all written values
		for(WriteLog::iterator iter = desc->write_log.begin();iter.hasNext();iter.next()) {
			WriteLogEntry &entry = *iter;
			
			// now update actual values
			WriteWordLogEntry *word_log_entry = entry.head;
			
			while(word_log_entry != NULL) {
				*word_log_entry->address = word_log_entry->MaskWord();
				word_log_entry = word_log_entry->next;
			}
			
			// release locks
			atomic_store_release(entry.read_lock, commitVersion);
			atomic_store_release(entry.write_lock, WRITE_LOCK_CLEAR);
		}
	} else {
		desc->stats.IncrementStatistics(StatisticsType::COMMIT_READ_ONLY);
	}
	
	atomic_store_release(&desc->tx_status, TX_COMMITTED);
	
#ifdef PRIVATIZATION_QUIESCENCE
	atomic_store_release(desc->quiescence_ts, MINIMUM_TS);
	desc->PrivatizationQuiescenceWait(ts);
#endif /* PRIVATIZATION_QUIESCENCE */
	
	if(!read_only) {
		desc->write_log.clear();
		desc->write_log_hashtable.clear();
	}
	
	desc->read_log.clear();
	
	// commit mem
	desc->mm.TxCommit<TransactionDynamic>(ts);
	
	desc->stats.IncrementStatistics(StatisticsType::COMMIT);
	desc->succ_aborts = 0;
	
	return COMMIT;				
}		

inline bool wlpdstm::TxLazy::LockWriteSet(TransactionDynamic *desc) {
	for(WriteLog::iterator iter = desc->write_log.begin();iter.hasNext();iter.next()) {
		WriteLogEntry &entry = *iter;
		WriteLock *write_lock = entry.write_lock;
		WriteLock lock_value = (WriteLock)atomic_load_no_barrier(write_lock);

		while(true) {
			while(is_write_locked(lock_value)) {
				if(desc->ShouldAbortWrite(write_lock)) {
					desc->stats.IncrementStatistics(StatisticsType::ABORT_WRITE_LOCKED);
					UnlockWriteSet(desc, iter);
					return false;
				} else {
					lock_value = (WriteLock)atomic_load_acquire(write_lock);
					desc->YieldCPU();
				}
			}
			
			if(atomic_cas_release(write_lock, WRITE_LOCK_CLEAR, &entry)) {
				entry.old_version = *(entry.read_lock);
				*(entry.read_lock) = READ_LOCK_SET;
				break;
			}
			
			lock_value = (WriteLock)atomic_load_acquire(write_lock);
			desc->YieldCPU();				
		}
	}

	return true;
}

inline void wlpdstm::TxLazy::UnlockWriteSet(TransactionDynamic *desc, WriteLog::iterator first_not_locked) {
	for(WriteLog::iterator iter = desc->write_log.begin();iter.hasNext();iter.next()) {
		if(iter == first_not_locked) {
			break;
		}

		WriteLogEntry &entry = *iter;
		// first write the old version and then mark the lock clear
		*entry.read_lock = entry.old_version;
		atomic_store_release(entry.write_lock, WRITE_LOCK_CLEAR);
	}
}

inline wlpdstm::TxLazy::TryCommitResult wlpdstm::TxLazy::TxTryCommitReadOnly(TransactionDynamic *desc) {
	atomic_store_release(&desc->tx_status, TX_COMMITTED);
	
#ifdef PRIVATIZATION_QUIESCENCE
	atomic_store_release(desc->quiescence_ts, MINIMUM_TS);
	desc->PrivatizationQuiescenceWait(ts);
#endif /* PRIVATIZATION_QUIESCENCE */

	desc->read_log.clear();
	
	// commit mem
	desc->mm.TxCommit<TransactionDynamic>(desc->valid_ts);

	desc->stats.IncrementStatistics(StatisticsType::COMMIT_READ_ONLY);
	desc->stats.IncrementStatistics(StatisticsType::COMMIT);
	desc->succ_aborts = 0;
	
	return COMMIT;	
}

inline void wlpdstm::TxLazy::TxCommitAfterTry(TransactionDynamic *desc, TryCommitResult result) {
	if(result == JUMP_RESTART) {
		desc->RestartJump();
	} else if(result == RESTART_RUNNING) {
		RestartRunning(desc);
	} else if(result == RESTART_COMMITTING) {
		RestartCommitting(desc);
	}

#ifdef PERFORMANCE_COUNTING
	if(desc->perf_cnt_sampling.should_sample()) {
		// if tx is restarted, this code is not reached
		desc->perf_cnt.TxEnd();
		desc->stats.IncrementStatistics(StatisticsType::CYCLES, desc->perf_cnt.GetElapsedCycles());
		desc->stats.IncrementStatistics(StatisticsType::RETIRED_INSTRUCTIONS, desc->perf_cnt.GetRetiredInstructions());
		desc->stats.IncrementStatistics(StatisticsType::CACHE_MISSES, desc->perf_cnt.GetCacheMisses());
	}
#endif /* PERFORMANCE_COUNTING */
	
	desc->stats.TxCommit();	
}

inline void wlpdstm::TxLazy::RollbackRunningInline(TransactionDynamic *desc) {
	if(desc->rolled_back) {
		return;
	}
	
	desc->rolled_back = true;	
	
	desc->read_log.clear();
	desc->write_log.clear();
	desc->write_log_hashtable.clear();
	
	desc->YieldCPU();
	desc->mm.TxAbort();
}

inline void wlpdstm::TxLazy::RollbackCommitting(TransactionDynamic *desc) {
	if(desc->rolled_back) {
		return;
	}
	
	desc->rolled_back = true;	
	
	for(WriteLog::iterator iter = desc->write_log.begin();iter.hasNext();iter.next()) {
		WriteLogEntry &entry = *iter;
		// first write the old version and then mark the lock clear
		*entry.read_lock = entry.old_version;
		atomic_store_release(entry.write_lock, WRITE_LOCK_CLEAR);
	}
	
	desc->read_log.clear();
	desc->write_log.clear();
	desc->write_log_hashtable.clear();
	
	desc->YieldCPU();
	desc->mm.TxAbort();
}
	
inline void wlpdstm::TxLazy::RestartRunning(TransactionDynamic *desc) {
	desc->profiling.TxRestartStart();
#ifdef PRIVATIZATION_QUIESCENCE
	*desc->quiescence_ts = MINIMUM_TS;
#endif /* PRIVATIZATION_QUIESCENCE */
	
	RollbackRunningInline(desc);
	atomic_store_release(&desc->tx_status, (Word)TX_RESTARTED);
	
#ifdef WAIT_ON_SUCC_ABORTS
	if(++desc->succ_aborts > SUCC_ABORTS_MAX) {
		desc->succ_aborts = SUCC_ABORTS_MAX;
	}
	
	if(desc->succ_aborts >= SUCC_ABORTS_THRESHOLD) {
		desc->WaitOnAbort();
	}
#endif /* WAIT_ON_SUCC_ABORTS */	
	
#ifdef PERFORMANCE_COUNTING
	if(desc->perf_cnt_sampling.should_sample()) {
		desc->perf_cnt.TxEnd();
		desc->stats.IncrementStatistics(StatisticsType::CYCLES, desc->perf_cnt.GetElapsedCycles());
		desc->stats.IncrementStatistics(StatisticsType::RETIRED_INSTRUCTIONS, desc->perf_cnt.GetRetiredInstructions());
		desc->stats.IncrementStatistics(StatisticsType::CACHE_MISSES, desc->perf_cnt.GetCacheMisses());
	}
#endif /* PERFORMANCE_COUNTING */
	desc->stats.IncrementStatistics(StatisticsType::ABORT);
	desc->profiling.TxRestartEnd();
	desc->stats.TxRestart();
	desc->RestartJump();
}

inline void wlpdstm::TxLazy::RestartCommitting(TransactionDynamic *desc) {
	desc->profiling.TxRestartStart();
#ifdef PRIVATIZATION_QUIESCENCE
	*desc->quiescence_ts = MINIMUM_TS;
#endif /* PRIVATIZATION_QUIESCENCE */
	
	RollbackCommitting(desc);
	atomic_store_release(&desc->tx_status, (Word)TX_RESTARTED);
	
#ifdef WAIT_ON_SUCC_ABORTS
	if(++desc->succ_aborts > SUCC_ABORTS_MAX) {
		desc->succ_aborts = SUCC_ABORTS_MAX;
	}
	
	if(desc->succ_aborts >= SUCC_ABORTS_THRESHOLD) {
		desc->WaitOnAbort();
	}
#endif /* WAIT_ON_SUCC_ABORTS */	
	
#ifdef PERFORMANCE_COUNTING
	if(desc->perf_cnt_sampling.should_sample()) {
		desc->perf_cnt.TxEnd();
		desc->stats.IncrementStatistics(StatisticsType::CYCLES, desc->perf_cnt.GetElapsedCycles());
		desc->stats.IncrementStatistics(StatisticsType::RETIRED_INSTRUCTIONS, desc->perf_cnt.GetRetiredInstructions());
		desc->stats.IncrementStatistics(StatisticsType::CACHE_MISSES, desc->perf_cnt.GetCacheMisses());
	}
#endif /* PERFORMANCE_COUNTING */
	desc->stats.IncrementStatistics(StatisticsType::ABORT);
	desc->profiling.TxRestartEnd();
	desc->stats.TxRestart();
	desc->RestartJump();
}
	
inline wlpdstm::WriteLogEntry *wlpdstm::TxLazy::LockMemoryStripeInline(TransactionDynamic *desc, WriteLock *write_lock) {
#ifdef DETAILED_STATS
	desc->stats.IncrementStatistics(StatisticsType::WRITES);
#endif /* DETAILED_STATS */

	VersionLock *read_lock = TransactionDynamic::map_write_lock_to_read_lock(write_lock);
	WriteLogEntry *ret = desc->write_log_hashtable.find((uintptr_t)read_lock);

	if(ret == NULL) {
#ifdef DETAILED_STATS
		desc->stats.IncrementStatistics(StatisticsType::NEW_WRITES);
#endif /* DETAILED_STATS */
		// prepare write log entry
		ret = desc->write_log.get_next();
		ret->write_lock = write_lock;
		ret->read_lock = read_lock;
		ret->ClearWordLogEntries(); // need this here TODO - maybe move this to commit/abort time
		ret->owner = desc; // this is for CM - TODO: try to move it out of write path

		desc->write_log_hashtable.insert((uintptr_t)read_lock, ret);
	}

	return ret;
}

// TODO: think about optimizing this by changing the ReadWord interface
//       - if ReadWord interface also had a mask as a parameter, the read and write could
//         skip reading after writing in some cases (write_float(addr1); read_floar(addr1);
//         on 64 bit machine comes to mind)
//       - alternatively ReadWord could return the mask that was read, which could be used
//         by functions calling ReadWord to decide whether to reread the whole word or not
inline Word wlpdstm::TxLazy::ReadWordInline(TransactionDynamic *desc, Word *address) {
	VersionLock *read_lock = TransactionDynamic::map_address_to_read_lock(address);
	WriteLogEntry *log_entry = desc->write_log_hashtable.find((uintptr_t)read_lock);
	
	// if locked by me return quickly
	if(log_entry != NULL) {
		WriteWordLogEntry *word_log_entry = log_entry->FindWordLogEntry(address);
		
		if(word_log_entry != NULL) {
			// if the whole word was written return from log
			if(word_log_entry->mask == LOG_ENTRY_UNMASKED) {
				return word_log_entry->value;
			}

			Word mem_word = ReadWordInnerLoop(desc, address, read_lock);
			return MaskWord(mem_word, word_log_entry->value, word_log_entry->mask);
		}
	}

	return ReadWordInnerLoop(desc, address, read_lock);
}

inline Word wlpdstm::TxLazy::ReadWordInnerLoop(TransactionDynamic *desc, Word *address, VersionLock *read_lock) {
	VersionLock version = (VersionLock)atomic_load_acquire(read_lock);
	Word value;
	
	while(true) {
		if(is_read_locked(version)) {
			version = (VersionLock)atomic_load_acquire(read_lock);
			desc->YieldCPU();
			continue;
		}
		
		value = (Word)atomic_load_acquire(address);
		VersionLock version_2 = (VersionLock)atomic_load_acquire(read_lock);
		
		if(version != version_2) {
			version = version_2;
			desc->YieldCPU();
			continue;
		}
		
		ReadLogEntry *entry = desc->read_log.get_next();
		entry->read_lock = read_lock;
		entry->version = version;		
		
		if(desc->ShouldExtend(version)) {
			if(!Extend(desc)) {
				// need to restart here
				desc->stats.IncrementStatistics(StatisticsType::ABORT_READ_VALIDATE);
				RestartRunning(desc);
			}
		}
		
		break;
	}
	
	return value;
}

inline bool wlpdstm::TxLazy::Extend(TransactionDynamic *desc) {
	unsigned ts = TransactionDynamic::commit_ts.readCurrentTsAcquire();
	
	if(desc->Validate()) {
		desc->valid_ts = ts;
#ifdef PRIVATIZATION_QUIESCENCE
		*desc->quiescence_ts = ts;
#endif /* PRIVATIZATION_QUIESCENCE */
		
#ifdef TS_EXTEND_STATS
		desc->stats.IncrementStatistics(StatisticsType::EXTEND_SUCCESS);
#endif /* TS_EXTEND_STATS */
		return true;
	}
	
#ifdef TS_EXTEND_STATS
	desc->stats.IncrementStatistics(StatisticsType::EXTEND_FAILURE);
#endif /* TS_EXTEND_STATS */
	
	return false;	
}

inline void wlpdstm::TxLazy::FirstWriteSetFunPtr(TransactionDynamic *desc) {
	desc->tx_commit_fun = TxCommit;
	desc->read_word_fun = ReadWord;
	desc->lock_memory_stripe_fun = LockMemoryStripe;
}

#ifdef WLPDSTM_TX_PROFILING_ADAPTIVE_DYNAMIC
inline void wlpdstm::TxLazy::FirstWriteSetFunPtrProfiled(TransactionDynamic *desc) {
	desc->tx_commit_fun = TxCommit;
	desc->read_word_fun = ReadWordProfiled;
	desc->lock_memory_stripe_fun = LockMemoryStripe;
}
#endif /* WLPDSTM_TX_PROFILING_ADAPTIVE_DYNAMIC */

#endif /* WLPDSTM_DYNAMIC_LAZY_IMPL_INLINE_H_ */
