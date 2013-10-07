/**
 * ****** Faith Emulator - Closed Source ******
 * Copyright (C) 2012 - 2013 Jean-Philippe Boivin
 *
 * Please read the WARNING, DISCLAIMER and PATENTS
 * sections in the LICENSE file.
 */

#include "log.h"
#include "finder.h"

using namespace std;

const char LogLevelName[] = { 'V', 'D', 'I', 'W', 'E', 'C' };

const size_t MAX_LOG_SIZE = 4096;


/* static */
Logger* Logger::sInstance = nullptr;

/* static */
Logger&
Logger :: getInstance()
{
    // TODO? Thread-safe
    if (sInstance == nullptr)
    {
        sInstance = new Logger();
    }
    return *sInstance;
}

/* static */
err_t
Logger :: init(const char* aDestFolder, const char* aPrefix)
{
    ASSERT_ERR(aDestFolder != nullptr && aDestFolder[0] != '\0', ERROR_INVALID_PARAMETER);
    ASSERT_ERR(aPrefix != nullptr && aPrefix[0] != '\0', ERROR_INVALID_PARAMETER);

    err_t err = ERROR_SUCCESS;

    Logger& logger = getInstance();
    if (logger.mDestFolder.empty() && logger.mPrefix.empty())
    {
        logger.mDestFolder = aDestFolder;
        logger.mPrefix = aPrefix;

        logger.start(QThread::LowestPriority);
    }
    else
    {
        logger.log(LOG_LEVEL_ERROR, __FILE__, __FUNCTION__, __LINE__,
                   "Logger already initialized.");
        err = ERROR_INVALID_STATE;
    }

    return err;
}

/* static */
bool
Logger :: willLog(LogLevel aLevel)
{
    if (sInstance != nullptr)
    {
        #ifdef DEBUG
        return aLevel >= LOG_LEVEL_DBG;
        #else
        return aLevel > LOG_LEVEL_DBG;
        #endif
    }
    return false;
}

/* static */
void
Logger :: log(LogLevel aLevel,
              const char* aFile, const char* aFunction, unsigned int aLine,
              const char* aFormat, ...)
{
    if (sInstance != nullptr && sInstance->isRunning())
    {
        va_list args;
        va_start(args, aFormat);

        sInstance->log(aLevel, aFile, aFunction, aLine, aFormat, args);
        va_end(args);
    }
}

Logger :: Logger()
{
    mStream = NULL;
    mCurrentDate = dateToInt(getCurrentTime());

    mHasToShutdown = false;
}

Logger :: ~Logger()
{
    if (isRunning())
    {
        mHasToShutdown = true;

        // notify in case the writing thread is waiting...
        mMutex.lock();
        mQueueNotEmpty.wakeAll();
        mMutex.unlock();

        wait();
    }

    if (mStream != nullptr)
    {
        fclose(mStream);
    }

    // free the queue, which should already be empty
    for (vector<LogData*>::iterator
            it = mQueue.begin(), end = mQueue.end();
         it != end; ++it)
    {
        LogData* data = *it;

        SAFE_DELETE_ARRAY(data->second);
        SAFE_DELETE(data);
    }

    sInstance = nullptr;
}

string
Logger :: getDestination()
{
    string dest;

    char buffer[16];
    snprintf(buffer, sizeof(buffer), "%d", mCurrentDate);

    dest += mDestFolder + mPrefix;
    dest += buffer;
    dest += ".log";

    return dest;
}

void
Logger :: log(LogLevel aLevel,
              const char* aFile, const char* aFunction, unsigned int aLine,
              const char* aFormat, va_list& aArgs)
{
    char* log = new char[MAX_LOG_SIZE];
    int len = 0;

    len += snprintf(log + len, MAX_LOG_SIZE - len, "%c/[%s:%s():%d] ",
                    LogLevelName[aLevel], aFile, aFunction, aLine);
    len += vsnprintf(log + len, MAX_LOG_SIZE - len,
                     aFormat, aArgs);
    len += snprintf(log + len, MAX_LOG_SIZE - len, "\n");

    tm time = getCurrentTime();
    LogData* data = new LogData(time, log);

    mMutex.lock();
    mQueue.push_back(data);
    mQueueNotEmpty.wakeAll();
    mMutex.unlock();
}

bool
Logger :: openLog(const char* aPath)
{
    ASSERT_ERR(aPath != nullptr, false);

    err_t err = ERROR_SUCCESS;

    if (mStream != nullptr)
    {
        fclose(mStream);
    }

    err = Finder::fileOpen(&mStream, aPath, "at");

    return ERROR_SUCCESS == err;
}

void
Logger :: run()
{
    string dest = getDestination();

    if (!openLog(dest.c_str()))
    {
        fprintf(stderr, "Failed to open the log file %s.", dest.c_str());
        perror(NULL);
        return;
    }

    while (true)
    {
        mMutex.lock();

        if (mHasToShutdown && mQueue.empty())
        {
            // shutdown has been called and the queue is now empty
            mMutex.unlock();
            break;
        }

        // while queue is empty, wait for the signal
        while (!mHasToShutdown && mQueue.empty())
        {
            mQueueNotEmpty.wait(&mMutex);
        }

        vector<LogData*> logs;
        if (!mQueue.empty())
        {
            // swap complexity is O(1)
            logs.swap(mQueue);
        }

        mMutex.unlock();

        // queue is now swapped...
        vector<LogData*>::iterator it, end;
        for (it = logs.begin(), end = logs.end();
             it != end; ++it)
        {
            LogData* data = *it;

            int date = dateToInt(data->first);
            if (date != mCurrentDate)
            {
                mCurrentDate = date;
                dest = getDestination();

                if (!openLog(dest.c_str()))
                {
                    fprintf(stderr, "Failed to open the log file %s.", dest.c_str());
                    perror(NULL);
                    break;
                }
            }

            char time[32];
            if (strftime(time, sizeof(time), "[%Y-%m-%d %H:%M:%S %Z]", &data->first) != 0)
            {
                if (fprintf(mStream, "%s ", time) < 0)
                {
                    perror("Failed to write log ");
                    break;
                }
            }

            if (fprintf(mStream, "%s ", data->second) < 0)
            {
                perror("Failed to write log ");
                break;
            }

            SAFE_DELETE_ARRAY(data->second);
            SAFE_DELETE(data);
        }

        // an error occured, free the memory and exit
        if (it != end)
        {
            --it;
            for (; it != end; ++it)
            {
                LogData* data = *it;

                SAFE_DELETE_ARRAY(data->second);
                SAFE_DELETE(data);
                return;
            }
        }

        fflush(mStream);

        yieldCurrentThread();
    }
}