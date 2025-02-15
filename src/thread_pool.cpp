/**************************************************************************
    Lightspark, a free flash player implementation

    Copyright (C) 2009-2013  Alessandro Pignotti (a.pignotti@sssup.it)

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU Lesser General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU Lesser General Public License for more details.

    You should have received a copy of the GNU Lesser General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
**************************************************************************/
#include <cassert>

#include "interfaces/threading.h"
#include "thread_pool.h"
#include "threading.h"
#include "timer.h"
#include "exceptions.h"
#include "compat.h"
#include "logger.h"
#include "swf.h"
#include "scripting/flash/system/flashsystem.h"

using namespace lightspark;

ThreadPool::ThreadPool(SystemState* s, size_t threads) :
threadPool(!s->runSingleThreaded ? threads : 0),
num_jobs(0),
stopFlag(false),
runcount(0)
{
	m_sys=s;
	size_t i = 0;
	for (auto& thread : threadPool)
	{
		thread.sys = s;
		thread.pool = this;
		thread.index = i++;
		thread.job = nullptr;
		thread.thread = SDL_CreateThread(job_worker,"ThreadPool",&thread);
	}
}

void ThreadPool::forceStop()
{
	if(!stopFlag)
	{
		stopFlag=true;
		//Signal an event for all the threads
		for (size_t i = 0; i < threadPool.size(); ++i)
			num_jobs.signal();

		{
			Locker l(mutex);
			//Now abort any job that is still executing
			for (auto thread : threadPool)
			{
				if (thread.job != nullptr)
				{
					thread.job->threadAborting = true;
					thread.job->threadAbort();
				}
			}
			//Fence all the non executed jobs
			for (auto job : jobs)
				job->jobFence();
			jobs.clear();
		}
		for (auto thread : threadPool)
			SDL_WaitThread(thread.thread, nullptr);

		for (auto pair : additionalThreads)
		{
			auto thread = pair.first;
			auto job = pair.second;

			if (job != nullptr)
			{
				job->threadAborting = true;
				job->threadAbort();
			}

			SDL_WaitThread(thread, nullptr);
		}
	}
}

void ThreadPool::waitAll()
{
	//Signal an event for all the threads
	for (size_t i = 0; i < threadPool.size(); ++i)
		num_jobs.signal();
	// wait for all threads to end
	for (auto thread : threadPool)
		SDL_WaitThread(thread.thread, nullptr);
	threadPool.clear();
	for (auto pair : additionalThreads)
	{
		auto thread = pair.first;
		SDL_WaitThread(thread, nullptr);
	}
	additionalThreads.clear();
}

ThreadPool::~ThreadPool()
{
	forceStop();
}

int ThreadPool::job_worker(void *d)
{
	Thread* thread = (Thread*)d;
	setTLSSys(thread->sys);

	ThreadProfile* profile=thread->sys->allocateProfiler(RGB(200,200,0));
	char buf[16];
	snprintf(buf,16,"Thread %u",(uint32_t)thread->index);
	profile->setTag(buf);

	Chronometer chronometer;
	while(1)
	{
		thread->pool->num_jobs.wait();
		if(thread->pool->stopFlag)
			return 0;
		Locker l(thread->pool->mutex);
		IThreadJob* myJob = thread->pool->jobs.front();
		thread->pool->jobs.pop_front();
		thread->job = myJob;
		thread->pool->runcount++;
		l.release();

		setTLSWorker(myJob->fromWorker);
		chronometer.checkpoint();
		try
		{
			// it's possible that a job was added and will be executed while forcestop() has been called
			if(thread->pool->stopFlag)
				return 0;
			myJob->execute();
		}
		catch(JobTerminationException& ex)
		{
			LOG(LOG_NOT_IMPLEMENTED,"Job terminated");
		}
		catch(LightsparkException& e)
		{
			LOG(LOG_ERROR,"Exception in ThreadPool " << e.what());
			thread->sys->setError(e.cause);
		}
		catch(std::exception& e)
		{
			LOG(LOG_ERROR,"std Exception in ThreadPool:"<<myJob<<" "<<e.what());
			thread->sys->setError(e.what());
		}
		
		profile->accountTime(chronometer.checkpoint());

		l.acquire();
		thread->job = nullptr;
		thread->pool->runcount--;
		l.release();

		//jobFencing is allowed to happen outside the mutex
		myJob->jobFence();
	}
	return 0;
}

void ThreadPool::addJob(IThreadJob* j)
{
	Locker l(mutex);
	j->setWorker(getWorker());
	if(stopFlag)
	{
		j->jobFence();
		return;
	}
	assert(j);
	if (runcount == threadPool.size())
	{
		// no slot available, we create an additional thread
		runAdditionalThread(j);
	}
	else
	{
		jobs.push_back(j);
		num_jobs.signal();
	}
}
void ThreadPool::runAdditionalThread(IThreadJob* j)
{
	additionalThreads.emplace_back(nullptr, nullptr);
	auto& thread = additionalThreads.back();
	thread.second = j;
	thread.first = SDL_CreateThread(additional_job_worker,"additionalThread",&thread);
}

int ThreadPool::additional_job_worker(void* d)
{
	ThreadPair* pair = (ThreadPair*)d;
	IThreadJob* myJob = pair->second;

	setTLSSys(myJob->fromWorker->getSystemState());
	setTLSWorker(myJob->fromWorker);
	try
	{
		myJob->execute();
	}
	catch(JobTerminationException& ex)
	{
		LOG(LOG_NOT_IMPLEMENTED,"Job terminated");
	}
	catch(LightsparkException& e)
	{
		LOG(LOG_ERROR,"Exception in AdditionalThread " << e.what());
		myJob->fromWorker->getSystemState()->setError(e.cause);
	}
	catch(std::exception& e)
	{
		LOG(LOG_ERROR,"std Exception in AdditionalThread:"<<myJob<<" "<<e.what());
		myJob->fromWorker->getSystemState()->setError(e.what());
	}
	myJob->jobFence();
	pair->second = nullptr;
	return 0;
}
