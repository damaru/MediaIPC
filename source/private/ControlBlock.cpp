#include "../public/ControlBlock.h"

#define MICROSECONDS (double)std::chrono::microseconds::period::den

namespace MediaIPC {

ControlBlock::ControlBlock()
{
	this->width = 0;
	this->height = 0;
	this->frameRate = 0;
	this->videoFormat = VideoFormat::None;

	this->maxWidth = 0;
	this->maxHeight = 0;
	this->mtime = 0;
	this->atime = 0;
	
	this->channels = 0;
	this->sampleRate = 0;
	this->samplesPerBuffer = 0;
	this->audioFormat = AudioFormat::None;
	
	this->active = false;
	this->lastBuffer = VideoBuffer::FrontBuffer;
	this->ringHead = 0;
}

uint64_t ControlBlock::calculateVideoBufsize() const
{
	if (this->videoFormat == VideoFormat::None) {
		return 0;
	}

	// backward compatibiliy
	if (!maxWidth  || !maxHeight) {
		return calculateVideoFramesize();
	}

	return this->maxWidth * this->maxHeight * FormatDetails::bytesPerPixel(this->videoFormat);
}

uint64_t ControlBlock::calculateVideoFramesize() const
{
	if (this->videoFormat == VideoFormat::None) {
		return 0;
	}
	
	return this->width * this->height * FormatDetails::bytesPerPixel(this->videoFormat);
}

uint64_t ControlBlock::calculateAudioBufsize() const
{
	if (this->audioFormat == AudioFormat::None) {
		return 0;
	}
	
	return this->channels * FormatDetails::bytesPerSample(this->audioFormat) * this->samplesPerBuffer;
}

std::chrono::microseconds ControlBlock::calculateVideoInterval() const
{
	double microseconds = (1.0 / (double)this->frameRate) * MICROSECONDS;
	return std::chrono::microseconds((uint64_t)microseconds);
}

std::chrono::microseconds ControlBlock::calculateAudioInterval() const
{
	double microseconds = (1.0 / ((double)this->sampleRate / (double)(this->samplesPerBuffer))) * MICROSECONDS;
	return std::chrono::microseconds((uint64_t)microseconds);
}

} //End MediaIPC
