/**
 * Copyright (C) 2012 Gang Liu <gangban.lau@gmail.com>
 * Copyright (C) 2008-2011 Teluu Inc. (http://www.teluu.com)
 * Copyright (C) 2003-2008 Benny Prijono <benny@prijono.org>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA 
 */

#include <pjmedia-audiodev/audiodev_imp.h>
#include <pjmedia/errno.h>
#include <pj/assert.h>
#include <pj/log.h>
#include <pj/os.h>
#include <pj/string.h>

#if defined(PJMEDIA_AUDIO_DEV_HAS_OPENSLES) && PJMEDIA_AUDIO_DEV_HAS_OPENSLES!=0

// __ANDROID_API__
#include <android/api-level.h>
#if __ANDROID_API__ >=14
#include <sys/system_properties.h>	// not a stable API
#endif

// for native audio
#include <SLES/OpenSLES.h>

#ifdef ANDROID
#include <SLES/OpenSLES_Android.h>
#include <SLES/OpenSLES_AndroidConfiguration.h>
#endif

#define MY_ASSERT_RETURN	PJ_ASSERT_RETURN
/*
#define MY_ASSERT_RETURN(expr, retval)  \
        do { \
                if (!(expr)) { assert(expr); return retval; } \
        } while (0)
*/

#define THIS_FILE	"OpenSLES_dev.c"
#define DRIVER_NAME	"OPENSLES"

typedef struct opensles_aud_factory
{
	pjmedia_aud_dev_factory base;
	pj_pool_factory *pf;
	pj_pool_t *pool;

	// engine interfaces
	SLObjectItf engineObject;
	SLEngineItf engineEngine;
#ifndef ANDROID
	SLboolean threadsafeMode;
	pj_mutex_t* engineMutex;
#endif

	SLboolean initialized;
} t_MyOpenSLESContext;

typedef t_MyOpenSLESContext *HAPPCONTEXT;

#define REC_BUF_SIZE_IN_SAMPLES		480	// Handle max 10 ms @ 48 kHz
#define PLAY_BUF_SIZE_IN_SAMPLES	480

// Number of the buffers in playout queue
#define N_PLAY_QUEUE_BUFFERS		2 * 5
// Number of buffers in recording queue
#define N_REC_QUEUE_BUFFERS		2 * 5
// Number of 10 ms recording blocks in rec buffer
#define N_REC_BUFFERS			20 * 5
// Number of 10 ms playout blocks in play buffer
#define N_PLAY_BUFFERS			5		// snd dev ptime max 60 ms

// Latency of recording queue item
#define N_REC_LATENCY	10	// 10 ms

/*
 * Sound stream descriptor.
 * This struct may be used for both unidirectional or bidirectional sound
 * streams.
 */
typedef struct opensles_aud_stream
{
	pjmedia_aud_stream base;

	pj_pool_t *pool;
	pj_str_t name;
	pjmedia_dir dir;
	pjmedia_aud_param param;

	void *user_data;
	pjmedia_aud_rec_cb rec_cb;
	pjmedia_aud_play_cb play_cb;

	HAPPCONTEXT appContext;

	SLuint32 frameBufSize;	// pjmedia_frame buf size, depend on audio dev ptime setting

	// Recording
	SLObjectItf recorderObject;
	SLRecordItf recorderRecord;
	SLAndroidSimpleBufferQueueItf recorderBufferQueue;
	SLboolean recInitialized;

	SLboolean recording;

	SLuint32 recChannelsCount;
	SLuint32 recSamplesPerSec;

	SLboolean recThreadIsInitialized;
	SLboolean recThreadQuit;
	pj_thread_t *recThread;
	pj_sem_t *recEvent;
	pj_mutex_t *recBufMutex;

	pj_thread_desc rec_thread_desc;
	pj_thread_t *recThreadReg;

	// Recording buffer
	//SLint8 recQueueBuffer[N_REC_QUEUE_BUFFERS][2 * REC_BUF_SIZE_IN_SAMPLES];
	SLint8 *recQueueBuffer[N_REC_QUEUE_BUFFERS];
	SLint8 recBuffer[N_REC_BUFFERS][2 * REC_BUF_SIZE_IN_SAMPLES];
	SLuint32 recLength[N_REC_BUFFERS];
	SLuint32 recSeqNumber[N_REC_BUFFERS];
	SLuint32 recCurrentSeq;
	// Current total size all data in buffers, used for delay estimate
	SLuint32 recBufferTotalSize;

	SLuint32 recQueueSeq;

	// playout
	SLObjectItf bqPlayerObject;
	SLPlayItf bqPlayerPlay;
	SLAndroidSimpleBufferQueueItf bqPlayerBufferQueue;
	SLObjectItf outputMixObject;
	SLboolean playInitialized;

	SLboolean playing;

	SLuint32 playChannelsCount;
	SLuint32 playSamplesPerSec;

	pj_thread_desc play_thread_desc;
	pj_thread_t *playThreadReg;

	// Playout buffer
	//SLint8 playQueueBuffer[N_PLAY_QUEUE_BUFFERS][2 * PLAY_BUF_SIZE_IN_SAMPLES];
	SLint8 *playQueueBuffer[N_PLAY_QUEUE_BUFFERS];
	SLuint32 playQueueSeq;
	SLint8 playBuffer[N_PLAY_BUFFERS][2 * PLAY_BUF_SIZE_IN_SAMPLES];
	SLuint32 playLength[N_PLAY_BUFFERS];

	pj_timestamp         play_timestamp;
	pj_timestamp         rec_timestamp;

} t_MyOpenSLESStreamContext;

typedef t_MyOpenSLESStreamContext *HSTREAMCONTEXT;

/* Factory prototypes */
static pj_status_t opensles_init(pjmedia_aud_dev_factory *f);
static pj_status_t opensles_destroy(pjmedia_aud_dev_factory *f);
static pj_status_t opensles_refresh(pjmedia_aud_dev_factory *f);
static unsigned opensles_get_dev_count(pjmedia_aud_dev_factory *f);
static pj_status_t opensles_get_dev_info(pjmedia_aud_dev_factory *f,
		unsigned index,
		pjmedia_aud_dev_info *info);
static pj_status_t opensles_default_param(pjmedia_aud_dev_factory *f,
		unsigned index,
		pjmedia_aud_param *param);
static pj_status_t opensles_create_stream(pjmedia_aud_dev_factory *f,
		const pjmedia_aud_param *param,
		pjmedia_aud_rec_cb rec_cb,
		pjmedia_aud_play_cb play_cb,
		void *user_data,
		pjmedia_aud_stream **p_aud_strm);

/* Stream prototypes */
static pj_status_t strm_get_param(pjmedia_aud_stream *strm,
		pjmedia_aud_param *param);
static pj_status_t strm_get_cap(pjmedia_aud_stream *strm,
		pjmedia_aud_dev_cap cap,
		void *value);
static pj_status_t strm_set_cap(pjmedia_aud_stream *strm,
		pjmedia_aud_dev_cap cap,
		const void *value);
static pj_status_t strm_start(pjmedia_aud_stream *strm);
static pj_status_t strm_stop(pjmedia_aud_stream *strm);
static pj_status_t strm_destroy(pjmedia_aud_stream *strm);

static pjmedia_aud_dev_factory_op opensles_op =
{
	&opensles_init,
	&opensles_destroy,
	&opensles_get_dev_count,
	&opensles_get_dev_info,
	&opensles_default_param,
	&opensles_create_stream,
    &opensles_refresh
};

static pjmedia_aud_stream_op opensles_strm_op =
{
	&strm_get_param,
	&strm_get_cap,
	&strm_set_cap,
	&strm_start,
	&strm_stop,
	&strm_destroy
};

#if 0
#include <android/cpu-features.h>
static void my_test()
{
	int  cpuCount = android_getCpuCount();
	PJ_LOG(3, (THIS_FILE, "cpuCount %d", cpuCount));
}
#endif

/*
Application callback handlers generally run in a restricted context, and should be written to perform their work quickly and then return as soon as possible. Do not do complex operations within a callback handler. For example, within a buffer queue completion callback, you can enqueue another buffer, but do not create an audio player. 
*/
static void RecorderBufferQueueCallback(
		SLAndroidSimpleBufferQueueItf queueItf, void *pContext) 
{
	HSTREAMCONTEXT r_poStreamContext = (HSTREAMCONTEXT)pContext;
	SLresult res;
	pj_status_t status;

	if (!pj_thread_is_registered())
	{
		status = pj_thread_register("recCBThread", r_poStreamContext->rec_thread_desc, &r_poStreamContext->recThreadReg);
		if (status == PJ_SUCCESS)
			PJ_LOG(5,(THIS_FILE, "Recorder BufferQueueCallback thread registered"));
		else
			PJ_LOG(2,(THIS_FILE, "Recorder BufferQueueCallback thread register failed"));
	}

	if (r_poStreamContext->recording)
	{
#if 1
		SLAndroidSimpleBufferQueueState state;
        	res = (*r_poStreamContext->recorderBufferQueue)->GetState(r_poStreamContext->recorderBufferQueue, &state);
		if (res != SL_RESULT_SUCCESS )
		{
			PJ_LOG(5,(THIS_FILE, "recorderBufferQueue GetState failed"));
		}
		else
		{
			PJ_LOG(5,(THIS_FILE, "recorderBufferQueue GetState c%u, i%u", state.count, state.index));
		}
#endif

		// Insert all data in temp buffer into recording buffers
		// There is zero or one buffer partially full at any given time,
		// all others are full or empty
		// Full means filled with noSamp10ms samples.

		const unsigned int noSamp10ms = r_poStreamContext->recSamplesPerSec
				/ 100;
		unsigned int dataPos = 0;
		SLuint16 bufPos = 0;
		SLint16 insertPos = -1;
		unsigned int nCopy = 0; // Number of samples to copy

		status = pj_mutex_lock(r_poStreamContext->recBufMutex);
		if (status != PJ_SUCCESS)
			PJ_LOG(1, (THIS_FILE, "lock recBuffer failed"));

		while (dataPos < noSamp10ms * N_REC_LATENCY / 10) //REC_BUF_SIZE_IN_SAMPLES) //noSamp10ms)

		{
			// Loop over all recording buffers or until we find the partially
			// full buffer
			// First choice is to insert into partially full buffer,
			// second choice is to insert into empty buffer
			bufPos = 0;
			insertPos = -1;
			nCopy = 0;
			while (bufPos < N_REC_BUFFERS) 
			{
				if ((r_poStreamContext->recLength[bufPos] > 0)
						&& (r_poStreamContext->recLength[bufPos] < noSamp10ms)) 
				{
					// Found the partially full buffer
					PJ_LOG(2,(THIS_FILE, "Found the partially full buffer"));
					insertPos = bufPos;
					bufPos = N_REC_BUFFERS; // Don't need to search more
				} 
				else if ((-1 == insertPos)
						&& (0 == r_poStreamContext->recLength[bufPos])) 
				{
					// Found an empty buffer, but we need verify whether partially full buffer there
					insertPos = bufPos;
				}
				++bufPos;
			}

			if (insertPos > -1)
			{
				// We found a non-full buffer, copy data from the buffer queue
				// o recBuffer
				unsigned int dataToCopy = noSamp10ms * N_REC_LATENCY / 10 - dataPos;
				unsigned int currentRecLen =
						r_poStreamContext->recLength[insertPos];
				unsigned int roomInBuffer = noSamp10ms - currentRecLen;
				nCopy = (dataToCopy < roomInBuffer ? dataToCopy : roomInBuffer);
				//PJ_LOG(2,(THIS_FILE, "recQueueBuffer->recBuffer, currentRecLen %d, dataPos %d, nCopy %d", currentRecLen, dataPos, nCopy));
				memcpy(&r_poStreamContext->recBuffer[insertPos][currentRecLen],
						&r_poStreamContext->recQueueBuffer[r_poStreamContext->recQueueSeq][dataPos],
						nCopy * sizeof(short));
				if (0 == currentRecLen)
				{
					r_poStreamContext->recSeqNumber[insertPos] =
							r_poStreamContext->recCurrentSeq;
					++r_poStreamContext->recCurrentSeq;
				}
				r_poStreamContext->recBufferTotalSize += nCopy;
				// Has to be done last to avoid interrupt problems
				// between threads
				r_poStreamContext->recLength[insertPos] += nCopy;
				dataPos += nCopy;
			} 
			else 
			{
				// Didn't find a non-full buffer
				PJ_LOG(2,(THIS_FILE, "Could not insert into recording buffer"));
				dataPos = noSamp10ms * N_REC_LATENCY / 10; // Don't try to insert more
			}
		}

		status = pj_mutex_unlock(r_poStreamContext->recBufMutex);
		if (status != PJ_SUCCESS)
			PJ_LOG(1, (THIS_FILE, "unlock recBuffer failed"));

		// clean the queue buffer
		// Start with empty buffer
		memset(r_poStreamContext->recQueueBuffer[r_poStreamContext->recQueueSeq],
				0, 2 * REC_BUF_SIZE_IN_SAMPLES * N_REC_LATENCY / 10);
		// write the empty buffer to the queue
		res =
				(*r_poStreamContext->recorderBufferQueue)->Enqueue(
						r_poStreamContext->recorderBufferQueue,
						(void*) r_poStreamContext->recQueueBuffer[r_poStreamContext->recQueueSeq],
						2 * noSamp10ms * N_REC_LATENCY / 10);
		if (res != SL_RESULT_SUCCESS) 
		{
			PJ_LOG(1, (THIS_FILE, "enqueue recorderBufferQueue failed"));
			return;
		}
		// update the rec queue seq
		r_poStreamContext->recQueueSeq = (r_poStreamContext->recQueueSeq + 1)
				% N_REC_QUEUE_BUFFERS;
		// wake up the recording thread
		status = pj_sem_post(r_poStreamContext->recEvent);
		if (status != PJ_SUCCESS)
			PJ_LOG(1,(THIS_FILE, "sem post failed"));
	}
}

static void PlayerBufferQueueCallback(
		SLAndroidSimpleBufferQueueItf queueItf, void *pContext) 
{
	HSTREAMCONTEXT r_poStreamContext = (HSTREAMCONTEXT)pContext;
	SLresult res;
	pj_status_t status;
	int i = 0;
	int bufPos = -1;
	int playLen;

	const SLint32 r_nBufSize = r_poStreamContext->frameBufSize;
	const unsigned int noSamp10ms = r_poStreamContext->playSamplesPerSec / 100;

	//Lock();
	/*
	PJ_LOG(5, (THIS_FILE, "playQueueSeq (%u)", r_poStreamContext->playQueueSeq));
	if (r_poStreamContext->playing)
		PJ_LOG(5, (THIS_FILE, "playing"));
	else
		PJ_LOG(5, (THIS_FILE, "not playing"));
	*/
	if (!pj_thread_is_registered())
	{
		status = pj_thread_register("playCBTH", r_poStreamContext->play_thread_desc, &r_poStreamContext->playThreadReg);
		if (status == PJ_SUCCESS)
			PJ_LOG(5,(THIS_FILE, "Playout BufferQueueCallback thread registered"));
		else
			PJ_LOG(2,(THIS_FILE, "Playout BufferQueueCallback thread register failed"));
	}

#if 1
	SLAndroidSimpleBufferQueueState state;
	res = (*r_poStreamContext->bqPlayerBufferQueue)->GetState(r_poStreamContext->bqPlayerBufferQueue, &state);
	if (res != SL_RESULT_SUCCESS )
	{
		PJ_LOG(5,(THIS_FILE, "bqPlayerBufferQueue GetState failed"));
	}
	else
	{
		PJ_LOG(5,(THIS_FILE, "bqPlayerBufferQueue GetState c%u, i%u", state.count, state.index));
	}
#endif

	if (r_poStreamContext->playing && (r_poStreamContext->playQueueSeq < N_PLAY_QUEUE_BUFFERS)) 
	{
		bufPos = -1;
		// check playBuffer first
		for (i = 0; i < N_PLAY_BUFFERS; i++)
		{
			if (r_poStreamContext->playLength[i] > 0)	// data available
			{
				playLen = r_poStreamContext->playLength[i];
				r_poStreamContext->playLength[i] = 0;
				bufPos = i;
				break;
			}
		}

		if (bufPos != -1) // playBuffer not empty
		{
			memcpy(r_poStreamContext->playQueueBuffer[r_poStreamContext->playQueueSeq], r_poStreamContext->playBuffer[bufPos], 
					2 * playLen);
		}
		else
		{
			//PJ_LOG(5, (THIS_FILE, "playout callback "));
			// Max 10 ms @ samplerate kHz / 16 bit
			//SLint8 playBuffer[2 * noSamp10ms];
			SLint8 playBuffer[r_nBufSize];
			//int noSamplesOut = 0;

			memset(playBuffer, 0x0, r_nBufSize);
			// Assumption for implementation
			// assert(PLAYBUFSIZESAMPLES == noSamp10ms);

			// TODO(xians), update the playout delay
			//UnLock();

			//noSamplesOut = _ptrAudioBuffer->RequestPlayoutData(noSamp10ms);
			//Lock();
			// Get data from Audio Device Buffer
			//noSamplesOut = _ptrAudioBuffer->GetPlayoutData(playBuffer);
			pjmedia_frame frame;

			frame.type = PJMEDIA_FRAME_TYPE_AUDIO;
			frame.buf = playBuffer;
			//frame.size = 2 * noSamp10ms;
			frame.size = r_nBufSize;
			//frame.timestamp.u64 = 0;
			frame.timestamp.u64 = r_poStreamContext->play_timestamp.u64;
			frame.bit_info = 0;

			//PJ_LOG(5, (THIS_FILE, "before play_cb"));
			status = (*r_poStreamContext->play_cb)(r_poStreamContext->user_data, &frame);		
			if (status != PJ_SUCCESS)
				PJ_LOG(2, (THIS_FILE, "play cb failed"));
			//PJ_LOG(5, (THIS_FILE, "after play_cb"));

			r_poStreamContext->play_timestamp.u64 += r_poStreamContext->param.samples_per_frame /
				r_poStreamContext->param.channel_count;

			if (frame.type != PJMEDIA_FRAME_TYPE_AUDIO)
			{
				PJ_LOG(2, (THIS_FILE, "play cb frame type not audio"));
				pj_bzero(frame.buf, frame.size);
			}

#if 0
			noSamplesOut = frame.size/2;
			// Cast OK since only equality comparison
			if (noSamp10ms != (unsigned int) noSamplesOut) 
			{
				PJ_LOG(2, (THIS_FILE,
							"noSamp10ms (%u) != noSamplesOut (%d)", noSamp10ms, noSamplesOut));

				//	if (_playWarning > 0) {
				//		WEBRTC_TRACE(kTraceWarning, kTraceAudioDevice, _id,
				//				"  Pending play warning exists");
				//	}
				//	_playWarning = 1;
			}
#endif
			// Insert what we have in data buffer
			//memcpy(r_poStreamContext->playQueueBuffer[r_poStreamContext->playQueueSeq], playBuffer, 2 * noSamplesOut);
			memcpy(r_poStreamContext->playQueueBuffer[r_poStreamContext->playQueueSeq], playBuffer, 2 * noSamp10ms);

			unsigned remainLen = frame.size - 2 * noSamp10ms;
			i = 0;
			SLint8 * ptr = playBuffer + 2 * noSamp10ms;
			while (remainLen > 0 && i < N_PLAY_BUFFERS)
			{
				r_poStreamContext->playLength[i] = noSamp10ms;
				memcpy(r_poStreamContext->playBuffer[i], ptr, 2 * noSamp10ms);
				remainLen -= 2 * noSamp10ms;
				ptr += 2 * noSamp10ms;
				i++;
			}
			//UnLock();

		}

		//PJ_LOG(5, (THIS_FILE, "playQueueSeq (%u)  noSamplesOut (%d)", r_poStreamContext->playQueueSeq, noSamplesOut));

		// write the buffer data we got from VoE into the device
		//res = (*r_poStreamContext->playerSimpleBufferQueue)->Enqueue(r_poStreamContext->playerSimpleBufferQueue,
		//		r_poStreamContext->playQueueBuffer[r_poStreamContext->playQueueSeq], 2 * noSamplesOut);
		res = (*r_poStreamContext->bqPlayerBufferQueue)->Enqueue(r_poStreamContext->bqPlayerBufferQueue,
				r_poStreamContext->playQueueBuffer[r_poStreamContext->playQueueSeq], 2 * noSamp10ms);
		if (res != SL_RESULT_SUCCESS) 
		{
			PJ_LOG(1, (THIS_FILE,
					" player simpler buffer queue Enqueue failed, %d",
					noSamp10ms));
			return;
		}
		// update the playout delay
		//UpdatePlayoutDelay(noSamplesOut);
		// update the play buffer sequency
		r_poStreamContext->playQueueSeq = (r_poStreamContext->playQueueSeq + 1) % N_PLAY_QUEUE_BUFFERS;
	}
	//PJ_LOG(5, (THIS_FILE, "playout callback ended"));
}

static pj_status_t opensles_to_pj_error(SLresult slesResult) 
{
	switch (slesResult)
	{
	case SL_RESULT_SUCCESS:
		return PJ_SUCCESS;

	case SL_RESULT_PRECONDITIONS_VIOLATED:
		return PJ_EINVALIDOP;

	case SL_RESULT_PARAMETER_INVALID:
		return PJ_EINVAL;

	case SL_RESULT_MEMORY_FAILURE:
	case SL_RESULT_BUFFER_INSUFFICIENT:
		return PJ_ENOMEM;

	case SL_RESULT_RESOURCE_ERROR:
	case SL_RESULT_IO_ERROR:
	case SL_RESULT_INTERNAL_ERROR:
		return PJMEDIA_EAUD_SYSERR;

	case SL_RESULT_RESOURCE_LOST:
	case SL_RESULT_CONTROL_LOST:
		return PJMEDIA_EAUD_NOTREADY;
	
	case SL_RESULT_CONTENT_CORRUPTED:
		return PJMEDIA_EAUD_SAMPFORMAT;

	case SL_RESULT_CONTENT_UNSUPPORTED:
	case SL_RESULT_FEATURE_UNSUPPORTED:
		return PJ_ENOTSUP;

	case SL_RESULT_UNKNOWN_ERROR:
		return PJMEDIA_EAUD_ERR;

	case SL_RESULT_OPERATION_ABORTED:
		return PJ_ECANCELLED;

	case SL_RESULT_CONTENT_NOT_FOUND:
		
	case SL_RESULT_PERMISSION_DENIED:
 
	default:
		return PJMEDIA_ERROR;
	}
}

#define MY_SUCCESS SL_RESULT_SUCCESS
#define MY_INVALID SL_RESULT_PARAMETER_INVALID

#define N_MAX_INTERFACES	4
#define N_MAX_OUTPUT_DEVICES	6
#define N_MAX_INPUT_DEVICES	3

static SLresult MyOpenSLES_IO_Init(HAPPCONTEXT r_poAppContext)
{
	MY_ASSERT_RETURN(r_poAppContext!=NULL, MY_INVALID);

	SLresult result;
/*
   Platform configuration
   OpenSL ES for Android is designed for multi-threaded applications, and is thread-safe.
   OpenSL ES for Android supports a single engine per application, and up to 32 objects. Available device memory and CPU may further restrict the usable number of objects.
   slCreateEngine recognizes, but ignores, these engine options:
   	SL_ENGINEOPTION_THREADSAFE
   	SL_ENGINEOPTION_LOSSOFCONTROL 
 */
	// create engine
#ifndef ANDROID
	r_poAppContext->threadsafeMode = SL_BOOLEAN_TRUE;

	SLEngineOption EngineOption[] = { { (SLuint32) SL_ENGINEOPTION_THREADSAFE,
			(SLuint32) SL_BOOLEAN_TRUE }, };
	result = slCreateEngine(&r_poAppContext->engineObject, 1, EngineOption, 0,
			NULL, NULL);
	if (SL_RESULT_SUCCESS != result)
	{
		// normally wouldn't go here!!!
		r_poAppContext->threadsafeMode = SL_BOOLEAN_FALSE;

		PJ_LOG(1, (THIS_FILE,
				"failed to create SL Engine Object in thread safe mode"));

		// try non-thread-safe mode
#endif
		result = slCreateEngine(&r_poAppContext->engineObject, 0, NULL, 0, NULL,
				NULL);
		if (SL_RESULT_SUCCESS != result)
		{
			PJ_LOG(1, (THIS_FILE,
#ifndef ANDROID
					"failed to create SL Engine Object in non-thread-safe mode"));
#else
					"failed to create SL Engine Object"));
#endif
			return result;
		}
#ifndef ANDROID
	}
#endif

	// realize the engine
	result = (*r_poAppContext->engineObject)->Realize(
			r_poAppContext->engineObject, SL_BOOLEAN_FALSE);
	if (SL_RESULT_SUCCESS != result)
	{
		PJ_LOG(1, (THIS_FILE, "failed to Realize SL Engine"));
		return result;
	}

#ifndef ANDROID
	SLboolean mutexCreated = SL_BOOLEAN_FALSE;
	if (!r_poAppContext->threadsafeMode)		// create our own mutex if non-thread-safe
	{
		pj_status_t status = pj_mutex_create(r_poAppContext->pool, "OpenSLESEngineMutex", PJ_MUTEX_RECURSE,
				&r_poAppContext->engineMutex);
		if (status != PJ_SUCCESS)
		{
			PJ_LOG(1, (THIS_FILE,
					"failed to Init thread mutex"));
			result = SL_RESULT_INTERNAL_ERROR;
			goto end_on_my_init_fail;
		}
		mutexCreated = SL_BOOLEAN_TRUE;
	}
#endif
	// get the engine interface, which is needed in order to create other objects
	result = (*r_poAppContext->engineObject)->GetInterface(
			r_poAppContext->engineObject, SL_IID_ENGINE,
			&r_poAppContext->engineEngine);
	if (SL_RESULT_SUCCESS != result)
	{
		PJ_LOG(1, (THIS_FILE,
				"failed to get SL Engine interface"));
		goto end_on_my_init_fail;
	}

	r_poAppContext->initialized = SL_BOOLEAN_TRUE;
	return MY_SUCCESS;

end_on_my_init_fail: 
	(*r_poAppContext->engineObject)->Destroy(
			r_poAppContext->engineObject);
	r_poAppContext->engineObject = NULL;
#ifndef ANDROID
	if (mutexCreated)
		pj_mutex_destroy(r_poAppContext->engineMutex);
#endif
	return result;
}

static int appcontext_lock(HAPPCONTEXT r_poAppContext)
{
#ifndef ANDROID
	int ret = 0;
	pj_status_t status;

	if (!r_poAppContext->threadsafeMode)
	{
		if ((status = pj_mutex_lock(r_poAppContext->engineMutex)) != PJ_SUCCESS)
		{
			PJ_LOG(1, (THIS_FILE,
					"failed to lock thread mutex"));
			ret = -1;
		}
	}

	return ret;
#else
	return 0;
#endif
}

static int appcontext_unlock(HAPPCONTEXT r_poAppContext)
{
#ifndef ANDROID
	int ret = 0;
	pj_status_t status;

	if (!r_poAppContext->threadsafeMode)
	{
		status = pj_mutex_unlock(r_poAppContext->engineMutex);
		if (status != PJ_SUCCESS)
			ret = -1;
	}
	return ret;
#else
	return 0;
#endif
}

static SLresult MyOpenSLES_IO_Terminate(HAPPCONTEXT r_poAppContext)
{
	if (!r_poAppContext->initialized)
		return MY_SUCCESS;

	MY_ASSERT_RETURN(appcontext_lock(r_poAppContext)==0,
			SL_RESULT_INTERNAL_ERROR);

	r_poAppContext->initialized = SL_BOOLEAN_FALSE;

	// TODO currently high level app need Stop recording, Stop playout first

	(*r_poAppContext->engineObject)->Destroy(r_poAppContext->engineObject);
	r_poAppContext->engineObject = NULL;

	appcontext_unlock(r_poAppContext);

#ifndef ANDROID
	if (!r_poAppContext->threadsafeMode)
	{
		pj_mutex_destroy(r_poAppContext->engineMutex);
	}
#endif

	return MY_SUCCESS;
}

/*
   PCM data format
   The PCM data format can be used with buffer queues only. Supported PCM playback configurations are 8-bit unsigned or 16-bit signed, mono or stereo, little endian byte ordering, and these sample rates: 8000, 11025, 12000, 16000, 22050, 24000, 32000, 44100, or 48000 Hz. For recording, the supported configurations are device-dependent, however generally 16000 Hz mono 16-bit signed is usually available.

   Note that the field samplesPerSec is actually in units of milliHz, despite the misleading name. To avoid accidentally using the wrong value, you should initialize this field using one of the symbolic constants defined for this purpose (such as SL_SAMPLINGRATE_44_1 etc.) 
 */
static SLboolean CheckSamplesPerSec(SLuint32 r_nSamplesPerSec,
		SLuint32 * r_pnSLValidSamplesPerSec)
{
	switch (r_nSamplesPerSec)
	{
	case 8000:
		*r_pnSLValidSamplesPerSec = SL_SAMPLINGRATE_8;
		break;
	case 11025:
		*r_pnSLValidSamplesPerSec = SL_SAMPLINGRATE_11_025;
		break;
	case 12000:
		*r_pnSLValidSamplesPerSec = SL_SAMPLINGRATE_12;
		break;
	case 16000:
		*r_pnSLValidSamplesPerSec = SL_SAMPLINGRATE_16;
		break;
	case 22050:
		*r_pnSLValidSamplesPerSec = SL_SAMPLINGRATE_22_05;
		break;
	case 24000:
		*r_pnSLValidSamplesPerSec = SL_SAMPLINGRATE_24;
		break;
	case 32000:
		*r_pnSLValidSamplesPerSec = SL_SAMPLINGRATE_32;
		break;
	case 44100:
		*r_pnSLValidSamplesPerSec = SL_SAMPLINGRATE_44_1;
		break;
	case 48000:
		*r_pnSLValidSamplesPerSec = SL_SAMPLINGRATE_48;
		break;
#ifndef ANDROID
	case 64000:
		*r_pnSLValidSamplesPerSec = SL_SAMPLINGRATE_64;
		break;
	case 88200:
		*r_pnSLValidSamplesPerSec = SL_SAMPLINGRATE_88_2;
		break;
	case 96000:
		*r_pnSLValidSamplesPerSec = SL_SAMPLINGRATE_96;
		break;
	case 192000:
		*r_pnSLValidSamplesPerSec = SL_SAMPLINGRATE_192;
		break;
#endif
	default:
		return SL_BOOLEAN_FALSE;
	}

	return SL_BOOLEAN_TRUE;
}

static SLresult MyOpenSLES_IO_InitPlayout(HAPPCONTEXT r_poAppContext,
		HSTREAMCONTEXT r_poStreamContext, SLuint32 r_nSamplesPerSec,
		SLuint32 r_nChannelsCount) 
{
	MY_ASSERT_RETURN(r_poAppContext!=NULL && r_poStreamContext!=NULL,
			MY_INVALID);
	MY_ASSERT_RETURN(r_poAppContext->initialized,
			SL_RESULT_PRECONDITIONS_VIOLATED);

	SLuint32 r_nSLValidSamplesPerSec;
	MY_ASSERT_RETURN(
			CheckSamplesPerSec(r_nSamplesPerSec, &r_nSLValidSamplesPerSec) == SL_BOOLEAN_TRUE,
			MY_INVALID);
	MY_ASSERT_RETURN(r_nChannelsCount == 1,
			MY_INVALID);

	MY_ASSERT_RETURN(appcontext_lock(r_poAppContext)==0,
			SL_RESULT_INTERNAL_ERROR);

	SLDataFormat_PCM pcm;
	SLDataSource audioSource;
	SLDataLocator_AndroidSimpleBufferQueue simpleBufferQueue;
	SLDataSink audioSink;
	SLDataLocator_OutputMix locator_outputmix;

	SLInterfaceID ids[N_MAX_INTERFACES];
	SLboolean req[N_MAX_INTERFACES];

	SLresult res = MY_SUCCESS;
	unsigned int i;

	SLboolean outputMixCreated = SL_BOOLEAN_FALSE;
	SLboolean bqPlayerObjectCreated = SL_BOOLEAN_FALSE;

	const SLint32 r_nBufSize = r_poStreamContext->frameBufSize;

	PJ_LOG(4, (THIS_FILE, "Playout frame buf size %u", r_nBufSize));

	if (r_poStreamContext->playing) 
	{
		PJ_LOG(2, (THIS_FILE, "Playout already started"));
		res = SL_RESULT_PRECONDITIONS_VIOLATED;
		goto end_on_my_initplay_fail;
	}

	// TODO device ID ? currently only use default device

	if (r_poStreamContext->playInitialized)
	{
		PJ_LOG(3, (THIS_FILE, "Playout already initialized"));
		goto end_on_my_initplay_fail;
	}

	// TODO Initialize the speaker ?

	// Create Output Mix object to be used by player
	for (i = 0; i < N_MAX_INTERFACES; i++) 
	{
		ids[i] = SL_IID_NULL;
		req[i] = SL_BOOLEAN_FALSE;
	}
	ids[0] = SL_IID_ENVIRONMENTALREVERB; // with environmental reverb specified as a non-required interface
	res = (*r_poAppContext->engineEngine)->CreateOutputMix(
			r_poAppContext->engineEngine, &r_poStreamContext->outputMixObject,
			1, ids, req);
	if (res != SL_RESULT_SUCCESS)
	{
		PJ_LOG(1, (THIS_FILE,
				"failed to get SL Output Mix object"));
		goto end_on_my_initplay_fail;
	}

	// Realizing the Output Mix object in synchronous mode.
	res = (*r_poStreamContext->outputMixObject)->Realize(
			r_poStreamContext->outputMixObject, SL_BOOLEAN_FALSE);
	if (res != SL_RESULT_SUCCESS) 
	{
		PJ_LOG(1, (THIS_FILE,
				"failed to realize SL Output Mix object"));
		goto end_on_my_initplay_fail;
	}

	outputMixCreated = SL_BOOLEAN_TRUE;

	// The code below can be moved to startplayout instead
	/* Setup the data source structure for the buffer queue */
	simpleBufferQueue.locatorType = SL_DATALOCATOR_ANDROIDSIMPLEBUFFERQUEUE;
	/* Two buffers in our buffer queue, to have low latency*/
	simpleBufferQueue.numBuffers = N_PLAY_QUEUE_BUFFERS;
	// TODO(xians), figure out if we should support stereo playout for android
	/* Setup the format of the content in the buffer queue */
	pcm.formatType = SL_DATAFORMAT_PCM;
	pcm.numChannels = r_nChannelsCount;
	// _samplingRateOut is initilized in InitSampleRate()
	pcm.samplesPerSec = r_nSLValidSamplesPerSec;
	pcm.bitsPerSample = SL_PCMSAMPLEFORMAT_FIXED_16;
	pcm.containerSize = SL_PCMSAMPLEFORMAT_FIXED_16;
	pcm.channelMask = SL_SPEAKER_FRONT_CENTER;	// TODO stereo SL_SPEAKER_FRONT_LEFT | SL_SPEAKER_FRONT_RIGHT
	pcm.endianness = SL_BYTEORDER_LITTLEENDIAN;
	audioSource.pFormat = (void *) &pcm;
	audioSource.pLocator = (void *) &simpleBufferQueue;
	/* Setup the data sink structure */
	locator_outputmix.locatorType = SL_DATALOCATOR_OUTPUTMIX;
	locator_outputmix.outputMix = r_poStreamContext->outputMixObject;
	audioSink.pLocator = (void *) &locator_outputmix;
	audioSink.pFormat = NULL;

	// Set arrays required[] and iidArray[] for SEEK interface
	// (PlayItf is implicit)
	ids[0] = SL_IID_BUFFERQUEUE;
	ids[1] = SL_IID_EFFECTSEND;
	ids[2] = SL_IID_VOLUME;
	ids[3] = SL_IID_ANDROIDCONFIGURATION;
	req[0] = SL_BOOLEAN_TRUE;
	req[1] = SL_BOOLEAN_TRUE;
	req[2] = SL_BOOLEAN_TRUE;
	req[3] = SL_BOOLEAN_TRUE;
	// Create the music player
	res = (*r_poAppContext->engineEngine)->CreateAudioPlayer(
			r_poAppContext->engineEngine, &r_poStreamContext->bqPlayerObject,
			&audioSource, &audioSink, 4, ids, req);
	if (res != SL_RESULT_SUCCESS) 
	{
		PJ_LOG(1, (THIS_FILE, "failed to create Audio Player"));
		goto end_on_my_initplay_fail;
	}

#ifdef ANDROID
	SLAndroidConfigurationItf playerConfig;
	PJ_LOG(5, (THIS_FILE, "before get player config"));
	res = (*r_poStreamContext->bqPlayerObject)->GetInterface(r_poStreamContext->bqPlayerObject, SL_IID_ANDROIDCONFIGURATION, &playerConfig);
	if (res != SL_RESULT_SUCCESS)
	{
		PJ_LOG(2, (THIS_FILE, "Can't get android configuration iface"));
		goto end_on_my_initplay_fail;
	}
	PJ_LOG(5, (THIS_FILE, "after get player config"));
//#if defined(MY_USE_DEV_EC) && MY_USE_DEC_EC!=0
	PJ_LOG(5, (THIS_FILE, "before set player config"));
	SLint32 streamType = SL_ANDROID_STREAM_VOICE;
	res = (*playerConfig)->SetConfiguration(playerConfig, SL_ANDROID_KEY_STREAM_TYPE, &streamType, sizeof(SLint32));
	if (res != SL_RESULT_SUCCESS)
	{
		PJ_LOG(2, (THIS_FILE, "Can't set audio player stream type to voice"));
	}
	PJ_LOG(5, (THIS_FILE, "after set player config"));
//#endif
#endif
	// Realizing the player in synchronous mode.
	res = (*r_poStreamContext->bqPlayerObject)->Realize(
			r_poStreamContext->bqPlayerObject, SL_BOOLEAN_FALSE);
	if (res != SL_RESULT_SUCCESS) 
	{
		PJ_LOG(1, (THIS_FILE, "failed to realize the player"));
		goto end_on_my_initplay_fail;
	}

	bqPlayerObjectCreated = SL_BOOLEAN_TRUE;

	// Get seek and play interfaces
	res = (*r_poStreamContext->bqPlayerObject)->GetInterface(
			r_poStreamContext->bqPlayerObject, SL_IID_PLAY,
			(void*) &r_poStreamContext->bqPlayerPlay);
	if (res != SL_RESULT_SUCCESS) 
	{
		PJ_LOG(1, (THIS_FILE, "failed to get Player interface"));
		goto end_on_my_initplay_fail;
	}
	res = (*r_poStreamContext->bqPlayerObject)->GetInterface(
			r_poStreamContext->bqPlayerObject, SL_IID_BUFFERQUEUE,
			(void*) &r_poStreamContext->bqPlayerBufferQueue);
	if (res != SL_RESULT_SUCCESS) 
	{
		PJ_LOG(1, (THIS_FILE,
				"failed to get Player Simple Buffer Queue interface"));
		goto end_on_my_initplay_fail;
	}

	// Setup to receive buffer queue event callbacks
	res = (*r_poStreamContext->bqPlayerBufferQueue)->RegisterCallback(
			r_poStreamContext->bqPlayerBufferQueue,
			PlayerBufferQueueCallback, r_poStreamContext);
	if (res != SL_RESULT_SUCCESS) 
	{
		PJ_LOG(1, (THIS_FILE,
				"failed to register Player Callback"));
		goto end_on_my_initplay_fail;
	}

	for (i = 0; i < N_PLAY_QUEUE_BUFFERS; i++)
		r_poStreamContext->playQueueBuffer[i] = pj_pool_alloc(r_poStreamContext->pool, r_nBufSize);

	r_poStreamContext->playChannelsCount = r_nChannelsCount;
	r_poStreamContext->playSamplesPerSec = r_nSamplesPerSec;

	r_poStreamContext->playInitialized = SL_BOOLEAN_TRUE;

	appcontext_unlock(r_poAppContext);
	PJ_LOG(3, (THIS_FILE, "InitPlayout OK"));
	return MY_SUCCESS;

end_on_my_initplay_fail:
	if (bqPlayerObjectCreated)
	{
		// Destroy the player
		(*r_poStreamContext->bqPlayerObject)->Destroy(
			r_poStreamContext->bqPlayerObject);
		r_poStreamContext->bqPlayerObject = NULL;
	}

	if (outputMixCreated)
	{
		// Destroy Output Mix object
		(*r_poStreamContext->outputMixObject)->Destroy(
			r_poStreamContext->outputMixObject);
		r_poStreamContext->outputMixObject = NULL;
	}
	 
	appcontext_unlock(r_poAppContext);
	PJ_LOG(3, (THIS_FILE, "InitPlayout failed"));
	return res;
}

static SLresult MyOpenSLES_IO_StartPlayout(HAPPCONTEXT r_poAppContext,
		HSTREAMCONTEXT r_poStreamContext) 
{
	MY_ASSERT_RETURN(r_poAppContext!=NULL && r_poStreamContext!=NULL,
			MY_INVALID);
	MY_ASSERT_RETURN(
			r_poAppContext->initialized && r_poStreamContext->playInitialized,
			SL_RESULT_PRECONDITIONS_VIOLATED);

	MY_ASSERT_RETURN(appcontext_lock(r_poAppContext)==0,
			SL_RESULT_INTERNAL_ERROR);

	SLresult res = MY_SUCCESS;
	SLuint32 nSample10ms = r_poStreamContext->playSamplesPerSec / 100;
	//SLint8 playBuffer[2 * nSample10ms];
	SLint8 playBuffer[r_poStreamContext->frameBufSize];
	SLuint32 noSamplesOut = 0;
	pjmedia_frame frame;
	int i;

	pj_status_t status = PJ_SUCCESS;

	if (r_poStreamContext->playing) 
	{
		PJ_LOG(3, (THIS_FILE, "Playing already started"));
		goto end_on_my_startplay_fail;
	}

	r_poStreamContext->playing = SL_BOOLEAN_TRUE;

	memset(r_poStreamContext->playLength, 0x0, sizeof(r_poStreamContext->playLength));

	memset(playBuffer, 0x0, sizeof(playBuffer));
	/* Enqueue a set of zero buffers to get the ball rolling */
	for (i = 0; i < (N_PLAY_QUEUE_BUFFERS - 1); i++)
	{
		/*
		frame.type = PJMEDIA_FRAME_TYPE_AUDIO;
		frame.buf = playBuffer;
		frame.size = r_poStreamContext->frameBufSize;
		frame.timestamp.u64 = 0;
		frame.bit_info = 0;
		status = (*r_poStreamContext->play_cb)(r_poStreamContext->user_data, &frame);
	
		noSamplesOut = frame.size/2;
		*/
		// Insert what we have in data buffer
		memcpy(r_poStreamContext->playQueueBuffer[r_poStreamContext->playQueueSeq], playBuffer, 2 * nSample10ms);
		
		// write the buffer data we got from upper into the device
		res = (*r_poStreamContext->bqPlayerBufferQueue)->Enqueue(r_poStreamContext->bqPlayerBufferQueue,
				(void*) r_poStreamContext->playQueueBuffer[r_poStreamContext->playQueueSeq], 2 * nSample10ms);
		if (res != SL_RESULT_SUCCESS) 
		{
			PJ_LOG(1, (THIS_FILE, "player simpler buffer queue Enqueue failed, %u",
					nSample10ms));
			//return ; dong return
		}
		r_poStreamContext->playQueueSeq = (r_poStreamContext->playQueueSeq + 1) % N_PLAY_QUEUE_BUFFERS;
	}

	// Play the PCM samples using a buffer queue
	res = (*r_poStreamContext->bqPlayerPlay)->SetPlayState(
			r_poStreamContext->bqPlayerPlay, SL_PLAYSTATE_PLAYING);
	if (res != SL_RESULT_SUCCESS) 
	{
		PJ_LOG(1, (THIS_FILE, "failed to start playout"));
		goto end_on_my_startplay_fail;
	}


	appcontext_unlock(r_poAppContext);
	PJ_LOG(4, (THIS_FILE, "start playout OK"));
	return MY_SUCCESS;

end_on_my_startplay_fail:
	r_poStreamContext->playing = SL_BOOLEAN_FALSE;
	appcontext_unlock(r_poAppContext);
	PJ_LOG(4, (THIS_FILE, "start playout failed"));
	return res;
}

// HERE
static SLresult MyOpenSLES_IO_StopPlayout(HAPPCONTEXT r_poAppContext,
		HSTREAMCONTEXT r_poStreamContext) 
{
	MY_ASSERT_RETURN(r_poAppContext!=NULL && r_poStreamContext!=NULL,
			MY_INVALID);
	MY_ASSERT_RETURN(r_poAppContext->initialized,
			SL_RESULT_PRECONDITIONS_VIOLATED);

	MY_ASSERT_RETURN(appcontext_lock(r_poAppContext)==0,
			SL_RESULT_INTERNAL_ERROR);

	SLresult res = MY_SUCCESS;

	if (!r_poStreamContext->playInitialized) 
	{
		PJ_LOG(3, (THIS_FILE, "Playing is not initialized"));
		goto end_on_my_stopplay_fail;
	}

	// Make sure player is stopped
	res = (*r_poStreamContext->bqPlayerPlay)->SetPlayState(
			r_poStreamContext->bqPlayerPlay, SL_PLAYSTATE_STOPPED);
	if (res != SL_RESULT_SUCCESS) 
	{
		PJ_LOG(1, (THIS_FILE, "failed to stop playout"));
	}
	res = (*r_poStreamContext->bqPlayerBufferQueue)->Clear(
			r_poStreamContext->bqPlayerBufferQueue);
	if (res != SL_RESULT_SUCCESS) 
	{
		PJ_LOG(1, (THIS_FILE,
				"failed to clear recorder buffer queue"));
	}

	// Destroy the player
	(*r_poStreamContext->bqPlayerObject)->Destroy(
			r_poStreamContext->bqPlayerObject);
	r_poStreamContext->bqPlayerObject = NULL;
	// Destroy Output Mix object
	(*r_poStreamContext->outputMixObject)->Destroy(
			r_poStreamContext->outputMixObject);
	r_poStreamContext->outputMixObject = NULL;

	r_poStreamContext->playInitialized = SL_BOOLEAN_FALSE;
	r_poStreamContext->playing = SL_BOOLEAN_FALSE;

	r_poStreamContext->playQueueSeq = 0;

	appcontext_unlock(r_poAppContext);
	PJ_LOG(4, (THIS_FILE, "stop playout OK"));
	return MY_SUCCESS;

end_on_my_stopplay_fail: 
	appcontext_unlock(r_poAppContext);
	PJ_LOG(4, (THIS_FILE, "stop playout failed"));
	return res;
}

static SLresult MyOpenSLES_IO_InitRecording(HAPPCONTEXT r_poAppContext,
		HSTREAMCONTEXT r_poStreamContext, SLuint32 r_nSamplesPerSec,
		SLuint32 r_nChannelsCount)
{
	MY_ASSERT_RETURN(r_poAppContext!=NULL && r_poStreamContext!=NULL,
			MY_INVALID);
	MY_ASSERT_RETURN(r_poAppContext->initialized,
			SL_RESULT_PRECONDITIONS_VIOLATED);

	SLuint32 r_nSLValidSamplesPerSec;
	MY_ASSERT_RETURN(
			CheckSamplesPerSec(r_nSamplesPerSec, &r_nSLValidSamplesPerSec) == SL_BOOLEAN_TRUE,
			MY_INVALID);
	MY_ASSERT_RETURN(r_nChannelsCount == 1,	// TODO mono now
			MY_INVALID);

	MY_ASSERT_RETURN(appcontext_lock(r_poAppContext)==0,
			SL_RESULT_INTERNAL_ERROR);

	int i;

	SLDataSource audioSource;
	SLDataLocator_IODevice micLocator;
	SLDataSink audioSink;
	SLDataFormat_PCM pcm;
	SLDataLocator_AndroidSimpleBufferQueue simpleBufferQueue;

	const SLInterfaceID id[2] = { SL_IID_ANDROIDSIMPLEBUFFERQUEUE, SL_IID_ANDROIDCONFIGURATION };
	const SLboolean req[2] = { SL_BOOLEAN_TRUE, SL_BOOLEAN_TRUE };

	SLresult res = MY_SUCCESS;
	
	SLboolean recorderObjectCreated = SL_BOOLEAN_FALSE;

	if (r_poStreamContext->recording)
	{
		PJ_LOG(2, (THIS_FILE, "Recording already started"));
		res = SL_RESULT_PRECONDITIONS_VIOLATED;
		goto end_on_my_initrec_fail;
	}

	// TODO device id ?

	if (r_poStreamContext->recInitialized) 
	{
		PJ_LOG(3, (THIS_FILE, "Recording already initialized"));
		goto end_on_my_initrec_fail;
	}

	// TODO init mic ?

	// Setup the data source structure
	micLocator.locatorType = SL_DATALOCATOR_IODEVICE;
	micLocator.deviceType = SL_IODEVICE_AUDIOINPUT;
	micLocator.deviceID = SL_DEFAULTDEVICEID_AUDIOINPUT; //micDeviceID;
	micLocator.device = NULL;
	audioSource.pLocator = (void *) &micLocator;
	audioSource.pFormat = NULL;

	/* Setup the data source structure for the buffer queue */
	simpleBufferQueue.locatorType = SL_DATALOCATOR_ANDROIDSIMPLEBUFFERQUEUE;
	simpleBufferQueue.numBuffers = N_REC_QUEUE_BUFFERS;
	/* Setup the format of the content in the buffer queue */
	pcm.formatType = SL_DATAFORMAT_PCM;
	pcm.numChannels = r_nChannelsCount;
	// _samplingRateIn is initialized in initSampleRate()
	pcm.samplesPerSec = r_nSLValidSamplesPerSec;
	pcm.bitsPerSample = SL_PCMSAMPLEFORMAT_FIXED_16;
	pcm.containerSize = 16;
	pcm.channelMask = SL_SPEAKER_FRONT_CENTER;	// TODO stereo SL_SPEAKER_FRONT_LEFT | SL_SPEAKER_FRONT_RIGHT
	pcm.endianness = SL_BYTEORDER_LITTLEENDIAN;
	audioSink.pFormat = (void *) &pcm;
	audioSink.pLocator = (void *) &simpleBufferQueue;

	res = (*r_poAppContext->engineEngine)->CreateAudioRecorder(
			r_poAppContext->engineEngine, &r_poStreamContext->recorderObject,
			&audioSource, &audioSink, 2, id, req);
	if (res != SL_RESULT_SUCCESS) 
	{
		PJ_LOG(1, (THIS_FILE, "failed to create Recorder"));
		goto end_on_my_initrec_fail;
	}

#ifdef ANDROID
	PJ_LOG(5, (THIS_FILE, "before get recorder config"));
	SLAndroidConfigurationItf recorderConfig;
	res = (*r_poStreamContext->recorderObject)->GetInterface(r_poStreamContext->recorderObject, SL_IID_ANDROIDCONFIGURATION, &recorderConfig);
	if (res != SL_RESULT_SUCCESS)
	{
		PJ_LOG(2, (THIS_FILE, "Can't get recorder config iface"));
		goto end_on_my_initrec_fail;
	}
	PJ_LOG(5, (THIS_FILE, "after get recorder config"));

//#if defined(MY_USE_DEV_EC) && MY_USE_DEC_EC!=0
	SLint32 streamType = SL_ANDROID_RECORDING_PRESET_GENERIC;

	// Precompil test is for NDK api use version.
	// Dynamic propriety test is for actual running on a device.
	// Both test are necessary to address build with older ndk and to address run with device from 9 <= runnning api < 14.
#if __ANDROID_API__ >= 14
	PJ_LOG(3, (THIS_FILE, "NDK build __ANDROID_API__ >=14"));
	char sdk_version[PROP_VALUE_MAX];
	int vlen =__system_property_get("ro.build.version.sdk", sdk_version);
	if (vlen > 0)
	{
		pj_str_t pj_sdk_version = pj_str(sdk_version);
		int sdk_v = pj_strtoul(&pj_sdk_version);
		if(sdk_v >= 14)
		{
			PJ_LOG(3, (THIS_FILE, "Runtime __ANDROID_API__ >=14"));
			streamType = SL_ANDROID_RECORDING_PRESET_VOICE_COMMUNICATION;
		}
	}
#endif
	//PJ_LOG(3, (THIS_FILE, "We have a stream type %d Cause SDK : %d", streamType, sdk_v));
	PJ_LOG(5, (THIS_FILE, "before set recorder config"));
	res = (*recorderConfig)->SetConfiguration(recorderConfig, SL_ANDROID_KEY_RECORDING_PRESET, &streamType, sizeof(SLint32));
	if (res != SL_RESULT_SUCCESS)
	{
		PJ_LOG(2, (THIS_FILE, "Can't set audio recorder stream type"));
		goto end_on_my_initrec_fail;
	}
	PJ_LOG(5, (THIS_FILE, "after set recorder config"));
//#endif
#endif

	// Realizing the recorder in synchronous mode.
	res = (*r_poStreamContext->recorderObject)->Realize(
			r_poStreamContext->recorderObject, SL_BOOLEAN_FALSE);
	if (res != SL_RESULT_SUCCESS) 
	{
		PJ_LOG(1, (THIS_FILE, "failed to realize Recorder"));
		goto end_on_my_initrec_fail;
	}

	recorderObjectCreated = SL_BOOLEAN_TRUE;

	// Get the RECORD interface - it is an implicit interface
	res = (*r_poStreamContext->recorderObject)->GetInterface(
			r_poStreamContext->recorderObject, SL_IID_RECORD,
			(void*) &r_poStreamContext->recorderRecord);
	if (res != SL_RESULT_SUCCESS) 
	{
		PJ_LOG(1, (THIS_FILE,
				"failed to get Recorder interface"));
		goto end_on_my_initrec_fail;
	}

	// Get the simpleBufferQueue interface
	res = (*r_poStreamContext->recorderObject)->GetInterface(
			r_poStreamContext->recorderObject, SL_IID_ANDROIDSIMPLEBUFFERQUEUE,
			(void*) &r_poStreamContext->recorderBufferQueue);
	if (res != SL_RESULT_SUCCESS) 
	{
		PJ_LOG(1, (THIS_FILE,
				"failed to get Recorder Simple Buffer Queue"));
		goto end_on_my_initrec_fail;
	}

	// Setup to receive buffer queue event callbacks
	res = (*r_poStreamContext->recorderBufferQueue)->RegisterCallback(
			r_poStreamContext->recorderBufferQueue,
			RecorderBufferQueueCallback, r_poStreamContext);
	if (res != SL_RESULT_SUCCESS) 
	{
		PJ_LOG(1, (THIS_FILE,
				"failed to register Recorder Callback"));
		goto end_on_my_initrec_fail;
	}

	for (i = 0; i < N_REC_QUEUE_BUFFERS; i++)
		r_poStreamContext->recQueueBuffer[i] = pj_pool_alloc(r_poStreamContext->pool, 2 * REC_BUF_SIZE_IN_SAMPLES * N_REC_LATENCY / 10);

	r_poStreamContext->recChannelsCount = r_nChannelsCount;
	r_poStreamContext->recSamplesPerSec = r_nSamplesPerSec;

	r_poStreamContext->recInitialized = SL_BOOLEAN_TRUE;

	appcontext_unlock(r_poAppContext);
	PJ_LOG(3, (THIS_FILE, "InitRecording OK"));
	return MY_SUCCESS;

end_on_my_initrec_fail: 
	if (recorderObjectCreated)
	{
		// Destroy the recorder object
		(*r_poStreamContext->recorderObject)->Destroy(
			r_poStreamContext->recorderObject);
		r_poStreamContext->recorderObject = NULL;
	}
	
	appcontext_unlock(r_poAppContext);
	PJ_LOG(3, (THIS_FILE, "InitRecording failed"));
	return res;
}

int MyOpenSLES_IO_PollRecordingProcess(void* arg)
{
	HSTREAMCONTEXT r_poStreamContext = (HSTREAMCONTEXT)arg;
	pj_status_t status;
	pjmedia_frame frame;
	int i;

	PJ_LOG(3, (THIS_FILE, "recThread started"));

	const SLint32 r_nBufSize = r_poStreamContext->frameBufSize;
	PJ_LOG(4, (THIS_FILE, "recording frame buf size %u", r_nBufSize));

	SLint32 frameCount = 0;			// how many frame buf ready, full
	SLint32 frameBufIdx = 0;
	SLint8 *frameBuffer[N_REC_BUFFERS];
	SLint32 frameBufLen[N_REC_BUFFERS];

	for (i = 0; i < N_REC_BUFFERS; i++)
	{
		frameBuffer[i] = (SLint8 *)pj_pool_alloc(r_poStreamContext->pool, r_nBufSize);
		frameBufLen[i] = 0;
	}

	while (!r_poStreamContext->recThreadQuit)
	{
		if (frameCount > 0) // have frame full
		{
			for (i = 0; i < frameCount; i++)
			{
				if (frameBufLen[i] == r_nBufSize)
				{
					if (r_poStreamContext->recording)
					{
						// submit pjmedia_frame
						frame.type = PJMEDIA_FRAME_TYPE_AUDIO;
						frame.buf = frameBuffer[i];
						frame.size = r_nBufSize;
						frame.timestamp.u64 = r_poStreamContext->rec_timestamp.u64;
						frame.bit_info = 0;

						status = (*r_poStreamContext->rec_cb)(r_poStreamContext->user_data, &frame);
						if (status != PJ_SUCCESS)
							PJ_LOG(1, (THIS_FILE, "rec cb failed"));

						r_poStreamContext->rec_timestamp.u64 += r_poStreamContext->param.samples_per_frame / r_poStreamContext->param.channel_count;

					}

				}
				else
				{
					PJ_LOG(2, (THIS_FILE, "frame buf size error"));
				}

				frameBufLen[i] = 0;
			}

			if (frameBufLen[frameBufIdx] > 0)	// partially full buffer
			{
				// move last to first index
				memcpy(frameBuffer[0], frameBuffer[frameBufIdx], frameBufLen[frameBufIdx]);
				frameBufLen[0] = frameBufLen[frameBufIdx];
				frameBufLen[frameBufIdx] = 0;
			}

			frameCount = 0;
			frameBufIdx = 0;
		}

		// TODO
		//    Lock();
		// Wait for 100ms for the signal from device callback
		// In case no callback comes in 100ms, we check the buffer anyway
		status = pj_sem_wait(r_poStreamContext->recEvent);
		if (status != PJ_SUCCESS)
		{
			PJ_LOG(1, (THIS_FILE, "sem wait failed"));
			continue;
		}
		
		if (r_poStreamContext->recThreadQuit)
			break;

		int bufPos = 0;
		unsigned int lowestSeq = 0;
		int lowestSeqBufPos = 0;
		pj_bool_t foundBuf = PJ_TRUE;
		const unsigned int noSamp10ms = r_poStreamContext->recSamplesPerSec / 100;

		status = pj_mutex_lock(r_poStreamContext->recBufMutex);
		if (status != PJ_SUCCESS)
			PJ_LOG(1, (THIS_FILE, "lock recBuffer failed"));

		// TODO 20 ms?
		while (foundBuf) 
		{
			// Check if we have any buffer with data to insert into the
			// Audio Device Buffer,
			// and find the one with the lowest seq number
			foundBuf = PJ_FALSE;

			for (bufPos = 0; bufPos < N_REC_BUFFERS; ++bufPos) 
			{
				if (noSamp10ms == r_poStreamContext->recLength[bufPos]) 
				{
					if (!foundBuf) 
					{
						lowestSeq = r_poStreamContext->recSeqNumber[bufPos];
						lowestSeqBufPos = bufPos;
						foundBuf = PJ_TRUE;
					} 
					else if (r_poStreamContext->recSeqNumber[bufPos] < lowestSeq) 
					{
						lowestSeq = r_poStreamContext->recSeqNumber[bufPos];
						lowestSeqBufPos = bufPos;
					}
				}
			} // for

			// Insert data into the Audio Device Buffer if found any
			if (foundBuf) 
			{
				//UpdateRecordingDelay();
				// Set the recorded buffer
				//_ptrAudioBuffer->SetRecordedBuffer(_recBuffer[lowestSeqBufPos],
				//	noSamp10ms);

				// Don't need to set the current mic level in ADB since we only
				// support digital AGC,
				// and besides we cannot get or set the iPhone mic level anyway.
				// Set VQE info, use clockdrift == 0
				//_ptrAudioBuffer->SetVQEData(_playoutDelay, _recordingDelay, 0);

				// Deliver recorded samples at specified sample rate, mic level
				// etc. to the observer using callback
				//UnLock();
				//_ptrAudioBuffer->DeliverRecordedData();
				//Lock();

				memcpy(frameBuffer[frameBufIdx] + frameBufLen[frameBufIdx], r_poStreamContext->recBuffer[lowestSeqBufPos], 2 * noSamp10ms);
				frameBufLen[frameBufIdx] += 2 * noSamp10ms;

				if (frameBufLen[frameBufIdx] == r_nBufSize) // enough for one pjmedia_frame
				{
					frameBufIdx++;
					frameCount++;
				}
			#if 0	
				pjmedia_frame frame;

				frame.type = PJMEDIA_FRAME_TYPE_AUDIO;
				frame.buf = r_poStreamContext->recBuffer[lowestSeqBufPos];
				frame.size = 2 * noSamp10ms;
				frame.timestamp.u64 = 0;
				frame.bit_info = 0;

				status = (*r_poStreamContext->rec_cb)(r_poStreamContext->user_data, &frame);
				if (status != PJ_SUCCESS)
					PJ_LOG(1, (THIS_FILE, "rec cb failed"));
			#endif
				// Make buffer available
				r_poStreamContext->recSeqNumber[lowestSeqBufPos] = 0;
				r_poStreamContext->recBufferTotalSize -= r_poStreamContext->recLength[lowestSeqBufPos];
				// Must be done last to avoid interrupt problems between threads
				r_poStreamContext->recLength[lowestSeqBufPos] = 0;
			}

		} // while (foundBuf)
	  	//UnLock();
		status = pj_mutex_unlock(r_poStreamContext->recBufMutex);
		if (status != PJ_SUCCESS)
			PJ_LOG(1, (THIS_FILE, "unlock recBuffer failed"));

	}

	PJ_LOG(3, (THIS_FILE, "recThread ended"));
	return 0;
}

static pj_status_t StartRecThread(HSTREAMCONTEXT r_poStreamContext)
{
	pj_status_t status;

	MY_ASSERT_RETURN(r_poStreamContext->recThread==NULL, SL_RESULT_INTERNAL_ERROR);

	status = pj_mutex_create_recursive(r_poStreamContext->pool, "recBufMutex", &r_poStreamContext->recBufMutex);
	if (status != PJ_SUCCESS)
	{
		PJ_LOG(1, (THIS_FILE, "mutex create failed"));
		return status;
	}

//	status = pj_sem_create(r_poStreamContext->pool, "recThSem", 0, N_REC_BUFFERS, &r_poStreamContext->recEvent);
	status = pj_sem_create(r_poStreamContext->pool, "recThSem", 0, 1, &r_poStreamContext->recEvent);
	if (status != PJ_SUCCESS)
	{
		PJ_LOG(1, (THIS_FILE, "sem create failed"));
		pj_mutex_destroy(r_poStreamContext->recBufMutex);
		return status;
	}

	r_poStreamContext->recThreadQuit = SL_BOOLEAN_FALSE;

	status = pj_thread_create(r_poStreamContext->pool, "OpenSLESRecThread", MyOpenSLES_IO_PollRecordingProcess, r_poStreamContext, PJ_THREAD_DEFAULT_STACK_SIZE, 0, &r_poStreamContext->recThread);	
	if (status != PJ_SUCCESS)
	{
		r_poStreamContext->recThread = NULL;
		r_poStreamContext->recThreadIsInitialized = SL_BOOLEAN_FALSE;
		pj_sem_destroy(r_poStreamContext->recEvent);
		r_poStreamContext->recEvent = NULL;
		pj_mutex_destroy(r_poStreamContext->recBufMutex);
		r_poStreamContext->recBufMutex = NULL;
		return status;
	}

	r_poStreamContext->recThreadIsInitialized = SL_BOOLEAN_TRUE;

	return PJ_SUCCESS;
}

static void StopRecThread(HSTREAMCONTEXT r_poStreamContext)
{
	pj_status_t status;

	if (r_poStreamContext->recThreadIsInitialized)
	{
		r_poStreamContext->recThreadQuit = SL_BOOLEAN_TRUE;
	#if 1
		status = pj_sem_post(r_poStreamContext->recEvent);	// wake up recThread
		if (status != PJ_SUCCESS)
			PJ_LOG(1, (THIS_FILE, "sem post failed"));
	#endif
		do
		{
			status = pj_thread_join(r_poStreamContext->recThread);
		} while(status != PJ_SUCCESS);
		pj_thread_destroy(r_poStreamContext->recThread);
		r_poStreamContext->recThread = NULL;
		pj_sem_destroy(r_poStreamContext->recEvent);
		r_poStreamContext->recEvent = NULL;
		pj_mutex_destroy(r_poStreamContext->recBufMutex);
		r_poStreamContext->recThreadIsInitialized = SL_BOOLEAN_FALSE;	
	}
}

static SLresult MyOpenSLES_IO_StartRecording(HAPPCONTEXT r_poAppContext,
		HSTREAMCONTEXT r_poStreamContext) 
{
	MY_ASSERT_RETURN(r_poAppContext!=NULL && r_poStreamContext!=NULL,
			MY_INVALID);
	MY_ASSERT_RETURN(
			r_poAppContext->initialized && r_poStreamContext->recInitialized,
			SL_RESULT_PRECONDITIONS_VIOLATED);

	MY_ASSERT_RETURN(appcontext_lock(r_poAppContext)==0,
			SL_RESULT_INTERNAL_ERROR);

	SLresult res = MY_SUCCESS;
	SLuint32 nSample10ms;
	int i;
	//pj_status_t status = PJ_SUCCESS;

	if (r_poStreamContext->recording) 
	{
		PJ_LOG(3, (THIS_FILE, "Recording already started"));
		goto end_on_my_startrec_fail;
	}

	// reset recording buffer
	for (i = 0; i < N_REC_QUEUE_BUFFERS; i++)
		memset(r_poStreamContext->recQueueBuffer[i], 0x0,
			2 * REC_BUF_SIZE_IN_SAMPLES * N_REC_LATENCY / 10);
	r_poStreamContext->recQueueSeq = 0;

	// start recording thread
	if (StartRecThread(r_poStreamContext) != PJ_SUCCESS)
	{
		r_poStreamContext->recThreadIsInitialized = SL_BOOLEAN_FALSE;
		goto end_on_my_startrec_fail;
	}

	r_poStreamContext->recThreadIsInitialized = SL_BOOLEAN_TRUE;

	memset(r_poStreamContext->recBuffer, 0,
			sizeof(r_poStreamContext->recBuffer));
	memset(r_poStreamContext->recLength, 0,
			sizeof(r_poStreamContext->recLength));
	memset(r_poStreamContext->recSeqNumber, 0,
			sizeof(r_poStreamContext->recSeqNumber));
	r_poStreamContext->recCurrentSeq = 0;
	r_poStreamContext->recBufferTotalSize = 0;

#if 0
	// in case already recording, stop recording and clear buffer queue
	res = (*r_poStreamContext->recorderRecord)->SetRecordState(r_poStreamContext->recorderRecord, SL_RECORDSTATE_STOPPED);
	assert(SL_RESULT_SUCCESS == res);
	res = (*r_poStreamContext->recorderBufferQueue)->Clear(r_poStreamContext->recorderBufferQueue);
	assert(SL_RESULT_SUCCESS == res);
#endif
	// TODO 44100 ?
	// Enqueue N_REC_QUEUE_BUFFERS -1 zero buffers to get the ball rolling
	// find out how it behaves when the sample rate is 44100
	nSample10ms = r_poStreamContext->recSamplesPerSec / 100;
	for (i = 0; i < (N_REC_QUEUE_BUFFERS - 1); i++) 
	{
		// We assign 10ms buffer to each queue, size given in bytes.
		res =
				(*r_poStreamContext->recorderBufferQueue)->Enqueue(
						r_poStreamContext->recorderBufferQueue,
						(void*) r_poStreamContext->recQueueBuffer[r_poStreamContext->recQueueSeq],
						sizeof(short) * nSample10ms * N_REC_LATENCY / 10);	// TODO 20 ms?
		if (res != SL_RESULT_SUCCESS) 
		{
			PJ_LOG(1, (THIS_FILE,
					"failed to Enqueue Empty Buffer to recorder"));
			goto end_on_my_startrec_fail;
		}
		r_poStreamContext->recQueueSeq++;
	}

	r_poStreamContext->recording = SL_BOOLEAN_TRUE;

	// Record the audio
	res = (*r_poStreamContext->recorderRecord)->SetRecordState(
			r_poStreamContext->recorderRecord, SL_RECORDSTATE_RECORDING);
	if (res != SL_RESULT_SUCCESS) 
	{
		PJ_LOG(1, (THIS_FILE, "failed to start recording"));
		goto end_on_my_startrec_fail;
	}

	appcontext_unlock(r_poAppContext);
	PJ_LOG(4, (THIS_FILE, "start recording OK"));
	return MY_SUCCESS;

end_on_my_startrec_fail:
	r_poStreamContext->recording = SL_BOOLEAN_FALSE;

	StopRecThread(r_poStreamContext);
	appcontext_unlock(r_poAppContext);
	PJ_LOG(4, (THIS_FILE, "start recording failed"));
	return res;
}

static SLresult MyOpenSLES_IO_StopRecording(HAPPCONTEXT r_poAppContext,
		HSTREAMCONTEXT r_poStreamContext) 
{
	MY_ASSERT_RETURN(r_poAppContext!=NULL && r_poStreamContext!=NULL,
			MY_INVALID);
	MY_ASSERT_RETURN(r_poAppContext->initialized,
			SL_RESULT_PRECONDITIONS_VIOLATED);

	MY_ASSERT_RETURN(appcontext_lock(r_poAppContext)==0,
			SL_RESULT_INTERNAL_ERROR);

	SLresult res = MY_SUCCESS;
	//pj_status_t status;

	if (!r_poStreamContext->recInitialized) 
	{
		PJ_LOG(3, (THIS_FILE, "Recording is not initialized"));
		goto end_on_my_stoprec_fail;
	}

	// Stop recording thread
	PJ_LOG(4, (THIS_FILE, "before StopRecThread"));
	StopRecThread(r_poStreamContext);
	PJ_LOG(4, (THIS_FILE, "after StopRecThread"));

	// Stop Record the audio
	res = (*r_poStreamContext->recorderRecord)->SetRecordState(
			r_poStreamContext->recorderRecord, SL_RECORDSTATE_STOPPED);
	if (res != SL_RESULT_SUCCESS) 
	{
		PJ_LOG(1, (THIS_FILE, "failed to stop recording"));
	}
	res = (*r_poStreamContext->recorderBufferQueue)->Clear(
			r_poStreamContext->recorderBufferQueue);
	if (res != SL_RESULT_SUCCESS) 
	{
		PJ_LOG(1, (THIS_FILE,
				"failed to clear recorder buffer queue"));
	}

	// Destroy the recorder object
	(*r_poStreamContext->recorderObject)->Destroy(
			r_poStreamContext->recorderObject);
	r_poStreamContext->recorderObject = NULL;

	r_poStreamContext->recInitialized = SL_BOOLEAN_FALSE;
	r_poStreamContext->recording = SL_BOOLEAN_FALSE;

	appcontext_unlock(r_poAppContext);
	PJ_LOG(4, (THIS_FILE, "stop recording OK"));
	return MY_SUCCESS;

end_on_my_stoprec_fail: 
	appcontext_unlock(r_poAppContext);
	PJ_LOG(4, (THIS_FILE, "stop recording failed"));
	return res;
}

static SLboolean MyOpenSLES_IO_RecordingInitialized(HSTREAMCONTEXT r_poStreamContext) {
	return r_poStreamContext->recInitialized;
}

static SLboolean MyOpenSLES_IO_PlayoutInitialized(HSTREAMCONTEXT r_poStreamContext) {
	return r_poStreamContext->playInitialized;
}

/*
 * Init Android audio driver.
 */
pjmedia_aud_dev_factory* pjmedia_opensles_factory(pj_pool_factory *pf) 
{
	struct opensles_aud_factory *f;
	pj_pool_t *pool;

	//my_test();

	pool = pj_pool_create(pf, "opensles", 256, 256, NULL);
	f = PJ_POOL_ZALLOC_T(pool, struct opensles_aud_factory);
	f->pf = pf;
	f->pool = pool;
	f->base.op = &opensles_op;

	return &f->base;
}

/* API: Init factory */
static pj_status_t opensles_init(pjmedia_aud_dev_factory *f) 
{
	struct opensles_aud_factory *sles = (struct opensles_aud_factory*)f;

	SLint32 res = MyOpenSLES_IO_Init(sles);	
	if (res == SL_RESULT_SUCCESS)
	{
		//PJ_LOG(4,(THIS_FILE, "OpenSLES sound driver initialized"));
		return PJ_SUCCESS;
	}
	else
	{
		PJ_LOG(3,(THIS_FILE, "OpenSLES sound driver initialize failed"));
		return opensles_to_pj_error(res); 
	}
}

/* API: Destroy factory */
static pj_status_t opensles_destroy(pjmedia_aud_dev_factory *f) 
{
	struct opensles_aud_factory *sles = (struct opensles_aud_factory*)f;

	PJ_LOG(4,(THIS_FILE, "OpenSLES sound driver shutting down.."));

	MyOpenSLES_IO_Terminate(sles);

	if (sles->pool != NULL)
		pj_pool_release(sles->pool);

	return PJ_SUCCESS;
}

/* API: refresh the list of devices */
static pj_status_t opensles_refresh(pjmedia_aud_dev_factory *f)
{
    PJ_UNUSED_ARG(f);
    return PJ_ENOTSUP;
}

/* API: Get device count. */
static unsigned opensles_get_dev_count(pjmedia_aud_dev_factory *f) 
{
	PJ_UNUSED_ARG(f);

	// TODO
	PJ_LOG(4,(THIS_FILE, "Get dev count"));

	return 1;
}

/* API: Get device info. */
static pj_status_t opensles_get_dev_info(pjmedia_aud_dev_factory *f,
		unsigned index,
		pjmedia_aud_dev_info *info) 
{
	PJ_UNUSED_ARG(f);

	// TODO
	PJ_LOG(4,(THIS_FILE, "Get dev info"));

	pj_bzero(info, sizeof(*info));

	pj_ansi_strcpy(info->name, "Android OpenSLES Default");
	info->default_samples_per_sec = 16000;
	info->caps = PJMEDIA_AUD_DEV_CAP_INPUT_VOLUME_SETTING |
				 PJMEDIA_AUD_DEV_CAP_OUTPUT_VOLUME_SETTING;
	info->input_count = 1;
	info->output_count = 1;

	return PJ_SUCCESS;
}

/* API: fill in with default parameter. */
static pj_status_t opensles_default_param(pjmedia_aud_dev_factory *f,
		unsigned index,
		pjmedia_aud_param *param)
{
	PJ_LOG(4,(THIS_FILE, "Default params"));

	pjmedia_aud_dev_info adi;
	pj_status_t status;

	status = opensles_get_dev_info(f, index, &adi);
	if (status != PJ_SUCCESS)
		return status;

	pj_bzero(param, sizeof(*param));
	if (adi.input_count && adi.output_count) 
	{
		param->dir = PJMEDIA_DIR_CAPTURE_PLAYBACK;
		param->rec_id = index;
		param->play_id = index;
	} 
	else if (adi.input_count) 
	{
		param->dir = PJMEDIA_DIR_CAPTURE;
		param->rec_id = index;
		param->play_id = PJMEDIA_AUD_INVALID_DEV;
	} 
	else if (adi.output_count) 
	{
		param->dir = PJMEDIA_DIR_PLAYBACK;
		param->play_id = index;
		param->rec_id = PJMEDIA_AUD_INVALID_DEV;
	} 
	else 
	{
		return PJMEDIA_EAUD_INVDEV;
	}

	param->clock_rate = adi.default_samples_per_sec;
	param->channel_count = 1;
	param->samples_per_frame = adi.default_samples_per_sec * 10 / 1000;	// TODO currently 10 ms
	param->bits_per_sample = 16;
	param->flags = adi.caps;
	param->input_latency_ms = PJMEDIA_SND_DEFAULT_REC_LATENCY;
	param->output_latency_ms = PJMEDIA_SND_DEFAULT_PLAY_LATENCY;

	return PJ_SUCCESS;
}

/* API: create stream */
static pj_status_t opensles_create_stream(pjmedia_aud_dev_factory *f,
		const pjmedia_aud_param *param,
		pjmedia_aud_rec_cb rec_cb,
		pjmedia_aud_play_cb play_cb,
		void *user_data,
		pjmedia_aud_stream **p_aud_strm)
{
	struct opensles_aud_factory *sles = (struct opensles_aud_factory*)f;
	pj_pool_t *pool;
	struct opensles_aud_stream *stream;
	pj_status_t status = PJ_SUCCESS;

	PJ_LOG(4,(THIS_FILE, "Creating stream"));

	PJ_ASSERT_RETURN(play_cb && rec_cb && p_aud_strm, PJ_EINVAL);
	PJ_ASSERT_RETURN(param != NULL && param->channel_count == 1, PJ_EINVAL);	// TODO Only mono

	PJ_ASSERT_RETURN(sles != NULL && sles->initialized, PJ_EINVAL);

	/* Initialize our stream data */
	pool = pj_pool_create(sles->pf, "OpenSLESstream", 512, 512, NULL);
	if (!pool) 
		return PJ_ENOMEM;

	stream = PJ_POOL_ZALLOC_T(pool, struct opensles_aud_stream);
	stream->pool = pool;
	stream->rec_cb = rec_cb;
	stream->play_cb = play_cb;
	stream->user_data = user_data;	// TODO what it is?
	pj_memcpy(&stream->param, param, sizeof(*param));
	stream->dir = param->dir;
	pj_strdup2_with_null(pool, &stream->name, "Android OpenSLES");

#if 0
	int bufferSize =  param->samples_per_frame * param->channel_count * param->bits_per_sample / 8;

    /* Set our buffers */
    stream->playerBufferSize =  bufferSize;
    stream->playerBuffer = (char*) pj_pool_alloc(stream->pool, stream->playerBufferSize);
    stream->recorderBufferSize = bufferSize;
    stream->recorderBuffer = (char*) pj_pool_alloc(stream->pool, stream->recorderBufferSize);
#endif

	PJ_LOG(3, (THIS_FILE, "Create stream : %d samples/sec, %d samples/frame, %d channels",
			param->clock_rate,
			param->samples_per_frame,
			param->channel_count));

	SLresult resPlayout = MY_SUCCESS;
	SLresult resRecording = MY_SUCCESS;

	stream->frameBufSize = param->samples_per_frame * param->channel_count * param->bits_per_sample / 8;

	stream->recording = SL_BOOLEAN_FALSE;
	stream->playing = SL_BOOLEAN_FALSE;

	/* Open the stream */
	if (param->dir == PJMEDIA_DIR_PLAYBACK) 
	{
		resPlayout = MyOpenSLES_IO_InitPlayout(sles, stream, param->clock_rate, param->channel_count);
	} 
	else if (param->dir == PJMEDIA_DIR_CAPTURE) 
	{
		resRecording = MyOpenSLES_IO_InitRecording(sles, stream, param->clock_rate, param->channel_count);
	}
	else if (param->dir == PJMEDIA_DIR_CAPTURE_PLAYBACK)
	{
		resPlayout = MyOpenSLES_IO_InitPlayout(sles, stream, param->clock_rate, param->channel_count);
		if (resPlayout == SL_RESULT_SUCCESS)
			resRecording = MyOpenSLES_IO_InitRecording(sles, stream, param->clock_rate, param->channel_count);
	}
	else
	{
		pj_assert(!"Invalid direction!");
		status = PJ_EINVAL;
		goto end_on_create_stream_fail;
	}


	if (resPlayout !=  SL_RESULT_SUCCESS || resRecording != SL_RESULT_SUCCESS)
	{
		if (resPlayout == SL_RESULT_SUCCESS)
			MyOpenSLES_IO_StopPlayout(sles, stream);

		if (resRecording == SL_RESULT_SUCCESS)
			MyOpenSLES_IO_StopRecording(sles, stream);

		status = PJMEDIA_ERROR;
		goto end_on_create_stream_fail;
	}	

	stream->appContext = sles;

	*p_aud_strm = &stream->base;
	(*p_aud_strm)->op = &opensles_strm_op;

	return PJ_SUCCESS;

end_on_create_stream_fail:
	pj_pool_release(pool);
	return status;
}

/* API: Get stream parameters */
static pj_status_t strm_get_param(pjmedia_aud_stream *s,
		pjmedia_aud_param *pi)
{
	struct opensles_aud_stream *strm = (struct opensles_aud_stream*)s;
	PJ_ASSERT_RETURN(strm && pi, PJ_EINVAL);

	PJ_LOG(4,(THIS_FILE, "Get stream params"));

	pj_memcpy(pi, &strm->param, sizeof(*pi));

	return PJ_SUCCESS;
}

/* API: get capability */
static pj_status_t strm_get_cap(pjmedia_aud_stream *s,
		pjmedia_aud_dev_cap cap,
		void *pval)
{
	struct opensles_aud_stream *strm = (struct opensles_aud_stream*)s;

	PJ_ASSERT_RETURN(strm && pval, PJ_EINVAL);

	PJ_LOG(4,(THIS_FILE, "Get stream caps"));

	pj_status_t status = PJ_ENOTSUP;

	switch (cap) {
		case PJMEDIA_AUD_DEV_CAP_INPUT_VOLUME_SETTING:
			status = PJ_SUCCESS;
			break;
		case PJMEDIA_AUD_DEV_CAP_OUTPUT_VOLUME_SETTING:
			status = PJ_SUCCESS;
			break;
		default:
		break;
	}

	return status;
}

/* API: set capability */
static pj_status_t strm_set_cap(pjmedia_aud_stream *strm,
		pjmedia_aud_dev_cap cap,
		const void *value)
{
	PJ_UNUSED_ARG(strm);
	PJ_UNUSED_ARG(cap);
	PJ_UNUSED_ARG(value);
	PJ_LOG(4,(THIS_FILE, "Set stream cap"));
	return PJMEDIA_EAUD_INVCAP;
}

/* API: start stream. */
static pj_status_t strm_start(pjmedia_aud_stream *s)
{
	struct opensles_aud_stream *stream = (struct opensles_aud_stream*)s;
	SLresult res;

	PJ_LOG(4,(THIS_FILE, "Starting %s stream..", stream->name.ptr));

	if (MyOpenSLES_IO_RecordingInitialized(stream))
	{
		res = MyOpenSLES_IO_StartRecording(stream->appContext, stream);
	}

	if (res == SL_RESULT_SUCCESS && MyOpenSLES_IO_PlayoutInitialized(stream))
	{
		res = MyOpenSLES_IO_StartPlayout(stream->appContext, stream);
	}

	PJ_LOG(4,(THIS_FILE, "Done, result=%d", res));
	return (res==SL_RESULT_SUCCESS)?PJ_SUCCESS:opensles_to_pj_error(res);
}

/* API: stop stream. */
static pj_status_t strm_stop(pjmedia_aud_stream *s)
{
	struct opensles_aud_stream *stream = (struct opensles_aud_stream*)s;

	PJ_LOG(5,(THIS_FILE, "Stopping stream.."));

	if (MyOpenSLES_IO_RecordingInitialized(stream)) 
	{
		MyOpenSLES_IO_StopRecording(stream->appContext, stream);
	}

	if (MyOpenSLES_IO_PlayoutInitialized(stream))
	{
		MyOpenSLES_IO_StopPlayout(stream->appContext, stream);
	}

	PJ_LOG(4,(THIS_FILE, "Stopping Done"));

	return PJ_SUCCESS;
}

/* API: destroy stream. */
static pj_status_t strm_destroy(pjmedia_aud_stream *s)
{
	struct opensles_aud_stream *stream = (struct opensles_aud_stream*)s;

	PJ_LOG(4,(THIS_FILE, "Destroying stream"));

	strm_stop(s);

	pj_pool_release(stream->pool);
	PJ_LOG(3,(THIS_FILE, "Stream is destroyed"));

	return PJ_SUCCESS;
}

#endif	/* PJMEDIA_AUDIO_DEV_HAS_OPENSLES */

