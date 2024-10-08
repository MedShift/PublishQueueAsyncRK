
#include "PublishQueueAsyncRK.h"


Logger pubqLogger("app.pubq");

PublishQueueAsyncBase::PublishQueueAsyncBase() {

}

PublishQueueAsyncBase::~PublishQueueAsyncBase() {

}

void PublishQueueAsyncBase::setup() {

	if (system_thread_get_state(nullptr) != spark::feature::ENABLED) {
		pubqLogger.error("SYSTEM_THREAD(ENABLED) is required");
		return;
	}

	haveSetup = true;

	os_mutex_create(&mutex);

	thread = new Thread("PublishQueueAsync", threadFunctionStatic, this, OS_THREAD_PRIORITY_DEFAULT, 2048);

}

void PublishQueueAsyncBase::mutexLock() const {
	os_mutex_lock(mutex);
}

void PublishQueueAsyncBase::mutexUnlock() const {
	os_mutex_unlock(mutex);
}

void PublishQueueAsyncBase::logPublishQueueEventData(const void *data) const {
	const PublishQueueEventData *eventDataStruct = (const PublishQueueEventData *)data;
	const char *eventName = &((const char *)data)[sizeof(PublishQueueEventData)];
	const char *eventData = eventName + strlen(eventName) + 1;

	
	pubqLogger.trace("ttl=%d flags=0x%2x size=%d eventName=%s", eventDataStruct->ttl, (int)eventDataStruct->flags, (int)eventDataStruct->size, eventName);
	pubqLogger.trace("eventData=%s", eventData);
}


void PublishQueueAsyncBase::threadFunction() {
	// Call the stateHandler forever
	while(true) {
		stateHandler(*this);
		os_thread_yield();
	}
}

void PublishQueueAsyncBase::startState() {
	// If we had other initialization to do, this would be a good place to do it.

	// Ready to process events
	stateHandler = &PublishQueueAsyncBase::checkQueueState;
}


void PublishQueueAsyncBase::checkQueueState() {
	if (!pausePublishing && Particle.connected() && millis() - lastPublish >= 1010) {

		PublishQueueEventData *data = getOldestEvent();
		if (data) {
			// We have an event and can probably publish
			isSending = true;

			const char *buf = reinterpret_cast<const char *>(data);
			const char *eventName = &buf[sizeof(PublishQueueEventData)];
			const char *eventData = eventName;
			eventData += strlen(eventData) + 1;

			PublishFlags flags(PublishFlag(data->flags));

			pubqLogger.info("publishing %s %s ttl=%d flags=%x", eventName, eventData, data->ttl, flags.value());

			auto request = Particle.publish(eventName, eventData, data->ttl, flags);

			// Use this technique of looping because the future will not be handled properly
			// when waiting in a worker thread like this.
			while(!request.isDone()) {
				delay(1);
				if (!isSending) {
					pubqLogger.info("publish canceled");
					return;
				}
			}
			bool bResult = request.isSucceeded();
			if (bResult) {
				// Successfully published
				pubqLogger.info("published successfully");
				discardOldEvent(false);
			}
			else {
				// Did not successfully transmit, try again after retry time
				// This string is searched for in the automated test suite, if edited the
				// test suite must also be edited
				pubqLogger.info("publish failed, will retry in %lu ms", failureRetryMs);
				stateHandler = &PublishQueueAsyncBase::waitRetryState;
			}
			isSending = false;
			lastPublish = millis();
		}
		else {
			pubqLogger.info("DISCARD!");
			// There must be some number of events in the queue, even though lookup failed
			if (this->getNumEvents() > 5) 
				discardOldEvent(false);
		}
	}
	else {
		// Not cloud connected or can't publish yet (not connected or published too recently)
	}

}

void PublishQueueAsyncBase::waitRetryState() {
	if (millis() - lastPublish >= failureRetryMs) {
		stateHandler = &PublishQueueAsyncBase::checkQueueState;
	}
}


// [static]
void PublishQueueAsyncBase::threadFunctionStatic(void *param) {
	static_cast<PublishQueueAsync *>(param)->threadFunction();
}


PublishQueueAsyncRetained::PublishQueueAsyncRetained(uint8_t *retainedBuffer, uint16_t retainedBufferSize) :
		retainedBuffer(retainedBuffer), retainedBufferSize(retainedBufferSize) {

}

PublishQueueAsyncRetained::~PublishQueueAsyncRetained() {

}

void PublishQueueAsyncRetained::setup() {
// Initialize the retained buffer
	bool initBuffer = false;

	volatile PublishQueueHeader *hdr = reinterpret_cast<PublishQueueHeader *>(retainedBuffer);
	if (hdr->magic == PUBLISH_QUEUE_HEADER_MAGIC && hdr->size == retainedBufferSize) {
		// Calculate the next write offset
		uint8_t *end = &retainedBuffer[retainedBufferSize];

		pubqLogger.trace("retained numEvents=%d", (int)hdr->numEvents);

		nextFree = &retainedBuffer[sizeof(PublishQueueHeader)];
		for(uint16_t ii = 0; ii < hdr->numEvents; ii++) {
			nextFree = skipEvent(nextFree);
			if (nextFree > end) {
				// Overflowed buffer, must be corrupted
				initBuffer = true;
				break;
			}
		}
	}
	else {
		// Not valid
		initBuffer = true;
	}

	//initBuffer = true; // Uncomment to discard old data

	if (initBuffer) {
		hdr->magic = PUBLISH_QUEUE_HEADER_MAGIC;
		hdr->size = retainedBufferSize;
		hdr->numEvents = 0;
		nextFree = &retainedBuffer[sizeof(PublishQueueHeader)];
	}
	pubqLogger.trace("at init numEvents=%d nextFree=%d", (int)hdr->numEvents, (int)(nextFree - retainedBuffer));

	// Do superclass setup (starting the thread)
	PublishQueueAsyncBase::setup();
}

bool PublishQueueAsyncRetained::publishCommon(const char *eventName, const char *data, int ttl, PublishFlags flags1, PublishFlags flags2) {

	if (!haveSetup) {
		setup();
	}

	if (data == NULL) {
		data = "";
	}

	// Size is the size of the header (8 bytes), the two c-strings (with null terminators), rounded up to a multiple of 4
	size_t size = sizeof(PublishQueueEventData) + strlen(eventName) + strlen(data) + 2;
	if ((size % 4) != 0) {
		size += 4 - (size % 4);
	}

	pubqLogger.info("queueing eventName=%s data=%s ttl=%d flags1=%d flags2=%d size=%d", eventName, data, ttl, flags1.value(), flags2.value(), size);

	if  (size > (retainedBufferSize - sizeof(PublishQueueHeader))) {
		// Special case: event is larger than the retained buffer. Rather than throw out all events
		// before discovering this, check that case first
		return false;
	}

	while(true) {
		{
			StMutexLock lock(this);

			uint8_t *end = &retainedBuffer[retainedBufferSize];
			if ((size_t)(end - nextFree) >= size) {
				// There is room to fit this
				pubqLogger.trace("saving event at nextFree=%d", (int)(nextFree - retainedBuffer));

				PublishQueueEventData *eventData = reinterpret_cast<PublishQueueEventData *>(nextFree);
				eventData->ttl = ttl;
				eventData->flags = flags1.value() | flags2.value();
				eventData->size = size;

				char *cp = reinterpret_cast<char *>(nextFree);
				cp += sizeof(PublishQueueEventData);

				strcpy(cp, eventName);
				cp += strlen(cp) + 1;

				strcpy(cp, data);

				nextFree += size;


				PublishQueueHeader *hdr = reinterpret_cast<PublishQueueHeader *>(retainedBuffer);
				hdr->numEvents++;

				pubqLogger.trace("after saving numEvents=%d nextFree=%d end=%d", (int)hdr->numEvents, (int)(nextFree - retainedBuffer), retainedBufferSize);
				return true;
			}

			// If there's only one event, there's nothing left to discard, this event is too large
			// to fit with the existing first event (which we can't delete because it might be
			// in the process of being sent)
			PublishQueueHeader *hdr = reinterpret_cast<PublishQueueHeader *>(retainedBuffer);
			if (hdr->numEvents == 1) {
				return false;
			}
		}

		// Discard the oldest event (false) if we're not currently sending.
		// If we are sending (isSending=true), discard the second oldest event
		if (!discardOldEvent(isSending)) {
			// There isn't an event to discard, so we don't have enough room
			return false;
		}
	}

	// Not reached
	return false;
}


PublishQueueEventData *PublishQueueAsyncRetained::getOldestEvent() {
	// This entire function holds a mutex lock that's released when returning
	StMutexLock lock(this);
	PublishQueueEventData *eventData = NULL;

	volatile PublishQueueHeader *hdr = reinterpret_cast<PublishQueueHeader *>(retainedBuffer);
	if (hdr->numEvents > 0) {
		eventData = reinterpret_cast<PublishQueueEventData *>(&retainedBuffer[sizeof(PublishQueueHeader)]);
	}

	return eventData;
}

bool PublishQueueAsyncRetained::clearEvents() {

	// This entire function holds a mutex lock that's released when returning
	StMutexLock lock(this);

	PublishQueueHeader *hdr = reinterpret_cast<PublishQueueHeader *>(retainedBuffer);
	hdr->numEvents = 0;
	nextFree = &retainedBuffer[sizeof(PublishQueueHeader)];
	isSending = false;
	lastPublish = 0;

    pubqLogger.trace("clearEvents numEvents=%d size=%d", (int)hdr->numEvents, (int)hdr->size);

	return true;
}

uint8_t *PublishQueueAsyncRetained::skipEvent(uint8_t *start) {
	start += sizeof(PublishQueueEventData);
	start += strlen(reinterpret_cast<char *>(start)) + 1;
	start += strlen(reinterpret_cast<char *>(start)) + 1;

	// Align
	size_t offset = start - retainedBuffer;
	if ((offset % 4) != 0) {
		start += 4 - (offset % 4);
	}


	return start;
}


bool PublishQueueAsyncRetained::discardOldEvent(bool secondEvent) {
	// This entire function holds a mutex lock that's released when returning
	StMutexLock lock(this);

	PublishQueueHeader *hdr = reinterpret_cast<PublishQueueHeader *>(retainedBuffer);
	uint8_t *start = &retainedBuffer[sizeof(PublishQueueHeader)];
	uint8_t *end = &retainedBuffer[retainedBufferSize];

	if (secondEvent) {
		if (hdr->numEvents < 2) {
			return false;
		}
		start = skipEvent(start);
	}
	else {
		if (hdr->numEvents < 1) {
			return false;
		}
	}

	// Remove the event at start
	uint8_t *next = skipEvent(start);
	size_t len = next - start;

	pubqLogger.trace("discardOldestEvent secondEvent=%d start=%d next=%d nextFree=%d", (int)secondEvent, (int)(start - retainedBuffer), (int)(next - retainedBuffer), (int)(nextFree - retainedBuffer));

	size_t after = end - next;
	if (after > 0) {
		// Move events down
		memmove(start, next, after);
	}

	nextFree -= len;
	hdr->numEvents--;

	pubqLogger.trace("after discardOldestEvent numEvents=%d nextFree=%d", hdr->numEvents, (int)(nextFree - retainedBuffer));


	return true;
}

uint16_t PublishQueueAsyncRetained::getNumEvents() const {
	uint16_t numEvents = 0;

	{
		StMutexLock lock(this);

		PublishQueueHeader *hdr = reinterpret_cast<PublishQueueHeader *>(retainedBuffer);
		numEvents = hdr->numEvents;
	}

	return numEvents;
}


