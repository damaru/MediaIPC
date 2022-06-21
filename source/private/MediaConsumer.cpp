#include "../public/MediaConsumer.h"
#include "IPCUtils.h"
#include "MemoryUtils.h"
#include "ObjectNames.h"
#include "RingBuffer.h"
#include <chrono>
#include <thread>
#include <iostream>
#include <utility>

using std::chrono::high_resolution_clock;

namespace MediaIPC {

static inline uint64_t RtcTimeMs()
{
	using namespace std::chrono;
	return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

namespace
{
	MemoryWrapperPtr consumerMemory(const std::string& name) {
		return MemoryUtils::toPointer(IPCUtils::getMemoryOnceExists(name, ipc::read_write));
	}
	
	MutexWrapperPtr consumerMutex(const std::string& name) {
		return MemoryUtils::toPointer(IPCUtils::getNamedMutex(name, false));
	}
}

MediaConsumer::MediaConsumer(const std::string& prefix, std::unique_ptr<ConsumerDelegate>&& delegate, uint32_t id)
{
	//Take ownership of the supplied delegate
	this->delegate = std::move(delegate);
	
	//Resolve the names of our shared memory objects and mutexes
	ObjectNames names(prefix);
	
	//Wait for all of the shared memory objects to exist before dealing with the mutexes
	//(This gives the producer a chance to populate the initial values before the consumer can acquire any locks)
	this->controlBlockMemory = consumerMemory(names.controlBlockMemory);
	this->videoFrontBuffer = consumerMemory(names.videoFrontBuffer);
	this->videoBackBuffer = consumerMemory(names.videoBackBuffer);
	this->audioBuffer = consumerMemory(names.audioBuffer);
	
	//Retrieve the named mutexes
	this->statusMutex = consumerMutex(names.statusMutex);
	this->videoMutex = consumerMutex(names.videoMutex);
	this->frontBufferMutex = consumerMutex(names.frontBufferMutex);
	this->backBufferMutex = consumerMutex(names.backBufferMutex);
	this->audioMutex = consumerMutex(names.audioMutex);
	
	//Point our control block pointer to the shared memory containing the data
	this->controlBlock = (ControlBlock*)(this->controlBlockMemory->mapped->get_address());
	
	this->controlBlock->ConsumerId = id;
	
	//Wrap our ring buffer interface around the audio buffer
	this->ringBuffer.reset(new RingBuffer(
		(uint8_t*)(this->audioBuffer->mapped->get_address()),
		this->audioBuffer->mapped->get_size(),
		&(this->controlBlock->ringHead
	)));
	
	//Pass a copy of the initial control block data to our delegate
	ControlBlock cbTemp;
	{
		MutexLock lock(*this->statusMutex->mutex);
		std::memcpy(&cbTemp, this->controlBlock, sizeof(ControlBlock));
	}
	this->delegate->controlBlockReceived(cbTemp);
	
	//Start our sampling loops
	std::thread audioThread(std::bind(&MediaConsumer::audioLoop, this));
	std::thread videoThread(std::bind(&MediaConsumer::videoLoop, this));
	audioThread.join();
	videoThread.join();
}

//Needed so that client code doesn't require definitions for our forward-declared types
MediaConsumer::~MediaConsumer() {}

bool MediaConsumer::streamIsActive()
{
	bool active = false;
	{
		MutexLock lock(*this->statusMutex->mutex);
		std::memcpy(&active, &(this->controlBlock->active), sizeof(bool));
	}
	return active;
}

void MediaConsumer::videoLoop()
{
	//Don't bother sampling anything if no video data is being transmitted
	if (this->controlBlock->videoFormat == VideoFormat::None) {
		return;
	}
	
	//Allocate memory to hold the last sampled video framebuffer
	uint32_t videoBufsize = this->videoFrontBuffer->mapped->get_size();
	std::unique_ptr<uint8_t[]> videoTempBuf(new uint8_t[videoBufsize]);
	
	//Determine our sampling frequency
	auto samplingFrequency = this->controlBlock->calculateVideoInterval();
	
	//Determine our starting time
	high_resolution_clock::time_point lastSample = high_resolution_clock::now();
	high_resolution_clock::time_point nextSample = lastSample;
	
	uint32_t width = this->controlBlock->width;
	uint32_t height = this->controlBlock->height;

	//Loop until the producer stops streaming data
	while (this->streamIsActive() == true)
	{
		//Determine the time point for the next sampling iteration
		lastSample = nextSample;
		nextSample = lastSample + samplingFrequency;
		
		//Determine which video framebuffer to use
		VideoBuffer bufToUse = VideoBuffer::FrontBuffer;
		{
			MutexLock lock(*this->videoMutex->mutex);
			bufToUse = this->controlBlock->lastBuffer;
		}
		
		//Sample the video framebuffer
		size_t size = 0;
		{
			auto& mutex = (bufToUse == VideoBuffer::FrontBuffer) ? this->frontBufferMutex : this->backBufferMutex;
			auto& source = (bufToUse == VideoBuffer::FrontBuffer) ? this->videoFrontBuffer : this->videoBackBuffer;
			
			MutexLock lock(*mutex->mutex);
			MutexLock cblock(*this->statusMutex->mutex);

			if (this->controlBlock->mtime > this->controlBlock->atime) {
				this->controlBlock->atime = RtcTimeMs();

				size = this->controlBlock->calculateVideoFramesize();
				std::memcpy(videoTempBuf.get(), source->mapped->get_address(), size);

				if (width !=  this->controlBlock->width || height != this->controlBlock->height) {
					width = this->controlBlock->width;
					height = this->controlBlock->height;
					//Pass a copy of the initial control block data to our delegate
					this->delegate->controlBlockReceived(*this->controlBlock);
				}
			}
		}
		
		//Pass the sampled data to our delegate
		if (size) {
			this->delegate->videoFrameReceived((const uint8_t*)(videoTempBuf.get()), size);
		}

		//Sleep until our next iteration
		std::this_thread::sleep_until(nextSample);
	}
}

void MediaConsumer::audioLoop()
{
	//Don't bother sampling anything if no audio data is being transmitted
	if (this->controlBlock->audioFormat == AudioFormat::None) {
		return;
	}
	
	//Allocate memory to hold the last sampled audio samples
	uint32_t audioBufsize = this->audioBuffer->mapped->get_size();
	std::unique_ptr<uint8_t[]> audioTempBuf(new uint8_t[audioBufsize]);
	
	//Determine our sampling frequency
	auto samplingFrequency = this->controlBlock->calculateAudioInterval();
	
	//Determine our starting time
	high_resolution_clock::time_point lastSample = high_resolution_clock::now();
	high_resolution_clock::time_point nextSample = lastSample;
	
	//Loop until the producer stops streaming data
	while (this->streamIsActive() == true)
	{
		//Determine the time point for the next sampling iteration
		lastSample = nextSample;
		nextSample = lastSample + samplingFrequency;
		
		//Sample the audio buffer
		{
			MutexLock lock(*this->audioMutex->mutex);
			this->ringBuffer->read(audioTempBuf.get(), audioBufsize);
		}
		
		//Pass the sampled data to our delegate
		this->delegate->audioSamplesReceived((const uint8_t*)(audioTempBuf.get()), audioBufsize);
		
		//Sleep until our next iteration
		std::this_thread::sleep_until(nextSample);
	}
}

} //End MediaIPC
