//
// LibSourcey
// Copyright (C) 2005, Sourcey <http://sourcey.com>
//
// LibSourcey is is distributed under a dual license that allows free, 
// open source use and closed source use under a standard commercial
// license.
//
// Non-Commercial Use:
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <http://www.gnu.org/licenses/>.
// 
// Commercial Use:
// Please contact mail@sourcey.com
//


#include "Sourcey/Media/MotionDetector.h"


using namespace cv;
using namespace std;
using namespace Poco;


namespace Sourcey {
namespace Media {


MotionDetector::MotionDetector(const Options& options) : 
	_options(options),
	_processing(false),
	_motionLevel(0),
	_motionFrameCount(0),
	_motionCanStartAt(0), 
	_motionStartedAt(0), 
	_motionSegmentEndingAt(0)
{
	Log("debug") << "[MotionDetector:" << this <<"] Creating" << endl;
}


MotionDetector::~MotionDetector() 
{
	Log("debug") << "[MotionDetector:" << this <<"] Destroying" << endl;
		
	// Synchronization issues; wait for computational
	// tasks to finish before returning.
	while (isProcessing()) {
		Log("debug") << "[MotionDetector:" << this <<"] Waiting for computational tasks to finish." << endl;
		Thread::sleep(5);
	}

	Log("debug") << "[MotionDetector:" << this <<"] Destroying: OK" << endl;
}


void MotionDetector::onStreamStateChange(const PacketStreamState&)
{
	Log("debug") << "[MotionDetector:" << this <<"] Reset Stream State" << endl;
	
	setState(this, MotionDetectorState::Idle);

	FastMutex::ScopedLock lock(_mutex); 
	_motionLevel = 0;
	_motionFrameCount = 0;
	_motionCanStartAt = 0; 
	_motionStartedAt = 0; 
	_motionSegmentEndingAt = 0;
}


void MotionDetector::process(IPacket& packet)
{
	Log("trace") << "[MotionDetector:" << this <<"] Processing" << endl;

	Media::VideoPacket* vpacket = dynamic_cast<Media::VideoPacket*>(&packet);		
	if (!vpacket) {
		dispatch(this, packet);
		return;
	}
	
	if (vpacket->mat == NULL)
		throw Exception("Video packets must contain an OpenCV image.");

	VideoPacket opacket;
	cv::Mat& source = *vpacket->mat;
	cv::Mat mask(source.size(), CV_8UC1);
	{
		FastMutex::ScopedLock lock(_mutex); 
	
		_processing = true;
		_timestamp = (double)clock() / CLOCKS_PER_SEC; // vpacket->time;
		updateMHI(source);
		computeMotionState();	

		// Create the single channel mask.
		_mhi.convertTo(mask, CV_8UC1, 255.0 / _options.stableMotionLifetime, 
			(_options.stableMotionLifetime - _timestamp) * (255.0 / _options.stableMotionLifetime));

		opacket = VideoPacket(&mask);
		_processing = false;	

		//cv::imshow("Motion Image", mask);
		//cv::imshow("Mask Image", _mhi);
		//cv::waitKey(10);
	}

	// NOTE: Dispatched images are GRAY8 so encoders will
	// need to adjust the input pixel format accordingly.
	dispatch(this, opacket);
	
	Log("trace") << "[MotionDetector:" << this <<"] Processing: OK" << endl;
}


void MotionDetector::updateMHI(cv::Mat& source)
{
	//FastMutex::ScopedLock lock(_mutex); 

	cv::Mat grey; //(source.size(), CV_8UC1);
	cv::cvtColor(source, grey, CV_RGB2GRAY);

	if (_last.type() != CV_8UC1 ||
		_last.size() != grey.size())
		cv::cvtColor(source, _last, CV_RGB2GRAY);

	cv::absdiff(_last, grey, _silhouette);
	cv::threshold(_silhouette, _silhouette, _options.minPixelValue, 255, cv::THRESH_BINARY);
	_last = grey;	
		
	// Computing motion history	
	if (!_options.useFastAlgorithm) {
		if (_mhi.size() != source.size() || //CV_32FC1
			_mhi.type() != CV_32FC1)
			_mhi = cv::Mat(source.size(), CV_32FC1);

		cv::updateMotionHistory(_silhouette, _mhi, _timestamp, _options.stableMotionLifetime);
		_motionLevel = countNonZero(_mhi);
	}
}


void MotionDetector::computeMotionState() 
{ 
	Log("debug") << "[MotionDetector:" << this <<"] Update Motion State: " << state().toString() 
		<< ": " << _motionLevel << ":" << _options.motionThreshold << endl;
	
	//FastMutex::ScopedLock lock(_mutex); 

	time_t currentTime = time(0);
		
	switch (state().id()) {
	case MotionDetectorState::Idle:
		{
			// The motion detctor has just started, compute our 
			// start time and set our state to Waiting...
			_motionCanStartAt = currentTime + _options.preSurveillanceDelay;
			setState(this, MotionDetectorState::Waiting);
			
			Log("debug") << "[MotionDetector:" << this <<"] UpdateMotionState: Set to Waiting" << endl;
		}
		break;
	case MotionDetectorState::Waiting: 
		{
			// If the timer is expired then set our state back
			// to Vigilant...
			if (currentTime > _motionCanStartAt) {				
				setState(this, MotionDetectorState::Vigilant);			
				//Log("debug") << "[MotionDetector:" << this <<"] UpdateMotionState: Set to Vigilant" << endl;
			}
		}
		break;
	case MotionDetectorState::Vigilant:
		{
			// If motion threshold is exceeded then set our state
			// to Triggered...
			if (_motionLevel > _options.motionThreshold) {

				// We need x number of motion frames before we 
				// consider motion stable and trigger the alarm.				
		
				// Our current motion count will be valid for 
				// 'stable motion duration' of nanoseconds.
				if (_motionFrameCount == 0) _stopwatch.start();
				if (_stopwatch.elapsed() < _options.stableMotionLifetime * 1000000) {
					_motionFrameCount++;
					Log("debug") << "Motion Frames Detected: " << _motionFrameCount << endl;
										
					// Stable motion detected, set our state to Triggered
					if (_motionFrameCount >= _options.stableMotionNumFrames) {	
						_motionFrameCount = 0;
						_stopwatch.stop();
						_stopwatch.reset();						
						_motionStartedAt = currentTime;
						_motionSegmentEndingAt = min<time_t>(
							currentTime + _options.minTriggeredDuration, 				
							_motionStartedAt + _options.maxTriggeredDuration);
						setState(this, MotionDetectorState::Triggered);
						//Log("debug") << "[MotionDetector:" << this <<"] UpdateMotionState: Set to Triggered" << endl;
					}
				} else {
					_stopwatch.stop();
					_stopwatch.reset();
					_motionSegmentEndingAt = 0;
					_motionFrameCount = 0;
					Log("debug") << "Motion Timer Dead" << endl;
				}
				Log("debug") << "Motion Detected" << endl;				
			}
		}
		break;
	case MotionDetectorState::Triggered: 
		{
			// If the timer is expired then set our state back
			// to Waiting...
			if (currentTime > _motionSegmentEndingAt) {				
				_motionCanStartAt = currentTime + _options.postMotionEndedDelay;
				setState(this, MotionDetectorState::Waiting);
				Log("debug") << "[MotionDetector:" << this <<"] UpdateMotionState: Set to Waiting" << endl;
			}

			// If motion threshold is exceeded while triggered
			// then extend the timer...
			else if (_motionLevel > _options.motionThreshold) {
				_motionSegmentEndingAt = min<time_t>(
					currentTime + _options.minTriggeredDuration, 				
					_motionStartedAt + _options.maxTriggeredDuration);	
				Log("debug") << "[MotionDetector:" << this <<"] Triggered: Extending to: " << _motionSegmentEndingAt << endl;			
			}
		}
		break;
	}
}


int MotionDetector::motionLevel() const
{
	FastMutex::ScopedLock lock(_mutex); 
	return _motionLevel;
}


MotionDetector::Options& MotionDetector::options()
{
	FastMutex::ScopedLock lock(_mutex); 
	return _options;
}


bool MotionDetector::isProcessing() const 
{ 
	FastMutex::ScopedLock lock(_mutex); 
	return _processing;
}


bool MotionDetector::isRunning() const 
{ 
	FastMutex::ScopedLock lock(_mutex); 
	return state().id() > MotionDetectorState::Idle;
}


} } // namespace Sourcey::Media