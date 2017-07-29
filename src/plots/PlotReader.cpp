// ==========================================================================
// 
// creepMiner - Burstcoin cryptocurrency CPU and GPU miner
// Copyright (C)  2016-2017 Creepsky (creepsky@gmail.com)
// 
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 3 of the License, or
// (at your option) any later version.
// 
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.See the
// GNU General Public License for more details.
// 
// You should have received a copy of the GNU General Public License
// along with this program; if not, write to the Free Software Foundation,
// Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110 - 1301  USA
// 
// ==========================================================================

#include "PlotReader.hpp"
#include "MinerUtil.hpp"
#include "logging/MinerLogger.hpp"
#include "mining/MinerConfig.hpp"
#include <fstream>
#include "mining/Miner.hpp"
#include <Poco/NotificationQueue.h>
#include "PlotVerifier.hpp"
#include <Poco/Timestamp.h>
#include "logging/Output.hpp"
#include "Plot.hpp"
#include "logging/Performance.hpp"

Burst::GlobalBufferSize Burst::PlotReader::globalBufferSize;

void Burst::GlobalBufferSize::setMax(Poco::UInt64 max)
{
	Poco::FastMutex::ScopedLock lock{ mutex_ };
	max_ = max;
}

bool Burst::GlobalBufferSize::reserve(Poco::UInt64 size)
{
	// unlimited memory
	if (MinerConfig::getConfig().getMaxBufferSize() == 0)
		return true;

	Poco::FastMutex::ScopedLock lock{ mutex_ };
	
	if (size_ + size > max_)
		return false;

	size_ += size;
	return true;
}

void Burst::GlobalBufferSize::free(Poco::UInt64 size)
{
	// unlimited memory
	if (MinerConfig::getConfig().getMaxBufferSize() == 0)
		return;

	Poco::FastMutex::ScopedLock lock{ mutex_ };
	
	if (size > size_)
		size = size_;

	size_ -= size;
}

Poco::UInt64 Burst::GlobalBufferSize::getSize() const
{
	return size_;
}

Poco::UInt64 Burst::GlobalBufferSize::getMax() const
{
	return max_;
}

Burst::PlotReader::PlotReader(Miner& miner, std::shared_ptr<Burst::PlotReadProgress> progress,
	Poco::NotificationQueue& verificationQueue, Poco::NotificationQueue& plotReadQueue)
	: Task("PlotReader"), miner_(miner), progress_{progress}, verificationQueue_{&verificationQueue}, plotReadQueue_(&plotReadQueue)
{
	//scoopNum_ = miner_.getScoopNum();
	//gensig_ = miner_.getGensig();
	//plotList_ = &plotList;
}

void Burst::PlotReader::runTask()
{
	while (!isCancelled())
	{
		Poco::Notification::Ptr notification(plotReadQueue_->waitDequeueNotification());
		PlotReadNotification::Ptr plotReadNotification;

		if (notification)
			plotReadNotification = notification.cast<PlotReadNotification>();
		else
			break;

		START_PROBE_DOMAIN("PlotReader.ReadDir", plotReadNotification->dir)

		// only process the current block
		if (miner_.getBlockheight() != plotReadNotification->blockheight)
			continue;

		Poco::Timestamp timeStartDir;

		// check, if the incoming plot-read-notification is for the current round
		auto currentBlock = plotReadNotification->blockheight == miner_.getBlockheight();

		// put in all related plot files
		for (const auto& relatedPlotList : plotReadNotification->relatedPlotLists)
			for (const auto& relatedPlotFile : relatedPlotList.second)
				plotReadNotification->plotList.emplace_back(relatedPlotFile);

		for (auto plotFileIter = plotReadNotification->plotList.begin();
			plotFileIter != plotReadNotification->plotList.end() &&
			!isCancelled() &&
			currentBlock;
			++plotFileIter)
		{
			auto& plotFile = **plotFileIter;
			std::ifstream inputStream(plotFile.getPath(), std::ifstream::in | std::ifstream::binary);
			
			START_PROBE_DOMAIN("PlotReader.ReadFile", plotFile.getPath())
			Poco::Timestamp timeStartFile;

			if (!isCancelled() && inputStream.is_open())
			{
				//MinerLogger::write("reading plot file " + plotFile.getPath());

				const auto staggerChunkBytes = MinerConfig::getConfig().getMaxBufferSize() * 1024 * 1024 /
					MinerConfig::getConfig().getBufferChunkCount();
				const auto staggerChunkRest = plotFile.getStaggerScoopBytes() % staggerChunkBytes;
				const auto staggerChunks = plotFile.getStaggerScoopBytes() / staggerChunkBytes + (staggerChunkRest > 0 ? 1u : 0u);

				// check, if the incoming plot-read-notification is for the current round
				currentBlock = plotReadNotification->blockheight == miner_.getBlockheight();

				for (auto staggerBlock = 0ul; !isCancelled() && staggerBlock < plotFile.getStaggerCount() && currentBlock; ++staggerBlock)
				{
					// check, if the incoming plot-read-notification is for the current round
					currentBlock = plotReadNotification->blockheight == miner_.getBlockheight();

					for (auto staggerChunk = 0u; staggerChunk < staggerChunks && !isCancelled() && currentBlock; ++staggerChunk)
					{
						START_PROBE_DOMAIN("PlotReader.StaggerChunk", plotFile.getPath())
						auto memoryAcquired = false;

						size_t readSize = staggerChunkBytes;

						if (staggerChunk == staggerChunks - 1 && staggerChunkRest != 0)
							readSize = staggerChunkRest;

						START_PROBE_DOMAIN("PlotReader.AllocMemory", plotFile.getPath())
						while (!isCancelled() && !memoryAcquired && currentBlock)
						{
							memoryAcquired = globalBufferSize.reserve(readSize);
							currentBlock = plotReadNotification->blockheight == miner_.getBlockheight();
						}
						TAKE_PROBE_DOMAIN("PlotReader.AllocMemory", plotFile.getPath())

						// if the reader is cancelled, jump out of the loop
						if (isCancelled())
						{
							// but first give free the allocated memory
							if (memoryAcquired)
								globalBufferSize.free(readSize);

							continue;
						}

						// only send the data to the verifiers, if the memory could be acquired and it is the right block
						if (memoryAcquired && currentBlock)
						{
							START_PROBE_DOMAIN("PlotReader.PushWork", plotFile.getPath());
							const auto chunkOffset = staggerChunk * staggerChunkBytes;
							const auto staggerBlockOffset = staggerBlock * plotFile.getStaggerBytes();
							const auto staggerScoopOffset = plotReadNotification->scoopNum * plotFile.getStaggerScoopBytes();

							START_PROBE("PlotReader.CreateVerification");
							VerifyNotification::Ptr verification(new VerifyNotification{});
							verification->accountId = plotFile.getAccountId();
							verification->nonceStart = plotFile.getNonceStart();
							verification->block = plotReadNotification->blockheight;
							verification->inputPath = plotFile.getPath();
							verification->gensig = plotReadNotification->gensig;
							verification->buffer.resize(readSize / sizeof(ScoopData));
							verification->nonceRead = staggerBlock * plotFile.getStaggerSize() + readSize * staggerChunk;
							verification->baseTarget = plotReadNotification->baseTarget;
							verification->memorySize = readSize;
							TAKE_PROBE("PlotReader.CreateVerification");

							START_PROBE_DOMAIN("PlotReader.SeekAndRead", plotFile.getPath());
							inputStream.seekg(staggerBlockOffset + staggerScoopOffset + chunkOffset);
							//inputStream.read(reinterpret_cast<char*>(&verification->buffer[0]), staggerScoopSize);
							inputStream.read(reinterpret_cast<char*>(&verification->buffer[0]), readSize);
							TAKE_PROBE_DOMAIN("PlotReader.SeekAndRead", plotFile.getPath());

							START_PROBE("PlotReader.EnqueueWork");
							verificationQueue_->enqueueNotification(verification);
							TAKE_PROBE("PlotReader.EnqueueWork");

							if (MinerConfig::getConfig().isSteadyProgressBar() && progress_ != nullptr)
							{
								START_PROBE("PlotReader.Progress");
								progress_->add(readSize * Settings::ScoopPerPlot, plotReadNotification->blockheight);
								TAKE_PROBE("PlotReader.Progress");
							}
							TAKE_PROBE_DOMAIN("PlotReader.PushWork", plotFile.getPath());

							// check, if the incoming plot-read-notification is for the current round
							currentBlock = plotReadNotification->blockheight == miner_.getBlockheight();
						}
						// if the memory was acquired, but it was not the right block, give it free
						else if (memoryAcquired)
							globalBufferSize.free(staggerChunkBytes);
						// this should never happen.. no memory allocated, not cancelled, wrong block
						else;

						TAKE_PROBE_DOMAIN("PlotReader.StaggerChunk", plotFile.getPath())
					}
				}

				inputStream.close();

				// check, if the incoming plot-read-notification is for the current round
				currentBlock = plotReadNotification->blockheight == miner_.getBlockheight();

				if (!isCancelled() && currentBlock)
				{
					auto fileReadDiff = timeStartFile.elapsed();
					auto fileReadDiffSeconds = static_cast<float>(fileReadDiff) / 1000 / 1000;
					Poco::Timespan span{fileReadDiff};
				
    				auto plotListSize = plotReadNotification->plotList.size();
    
    				if (plotListSize > 0)
    				{
    					miner_.getData().getBlockData()->setProgress(
							plotReadNotification->dir,
    						static_cast<float>(std::distance(plotReadNotification->plotList.begin(), plotFileIter) + 1) / plotListSize * 100.f,
							plotReadNotification->blockheight
						);
    				}

					const auto nonceBytes = static_cast<float>(plotFile.getNonces() * Settings::ScoopSize);
					const auto bytesPerSeconds = nonceBytes / fileReadDiffSeconds;

					log_information_if(MinerLogger::plotReader, MinerLogger::hasOutput(PlotDone), "%s (%s) read in %ss (~%s/s)",
						plotFile.getPath(),
						memToString(plotFile.getSize(), 2),
						Poco::DateTimeFormatter::format(span, "%s.%i"),
						memToString(static_cast<Poco::UInt64>(bytesPerSeconds), 2));

					if (!MinerConfig::getConfig().isSteadyProgressBar() && progress_ != nullptr)
					{
						START_PROBE("PlotReader.Progress")
						progress_->add(plotFile.getSize(), plotReadNotification->blockheight);
						TAKE_PROBE("PlotReader.Progress")
					}
				}

				// if it was cancelled, we push the current plot dir back in the queue again
				if (isCancelled())
					plotReadQueue_->enqueueNotification(plotReadNotification);
			}

			TAKE_PROBE_DOMAIN("PlotReader.ReadFile", plotFile.getPath())
		}
		
		miner_.getData().getBlockData()->setProgress(plotReadNotification->dir, 100.f, plotReadNotification->blockheight);

		auto dirReadDiff = timeStartDir.elapsed();
		auto dirReadDiffSeconds = static_cast<float>(dirReadDiff) / 1000 / 1000;
		Poco::Timespan span{dirReadDiff};

		Poco::UInt64 totalSizeBytes = 0u;

		for (const auto& plot : plotReadNotification->plotList)
			totalSizeBytes += plot->getSize();
		
		if (plotReadNotification->type == PlotDir::Type::Sequential && totalSizeBytes > 0 && currentBlock)
		{
			const auto sumNonces = totalSizeBytes / Settings::PlotSize;
			const auto sumNoncesBytes = static_cast<float>(sumNonces * Settings::ScoopSize);
			const auto bytesPerSecond = sumNoncesBytes / dirReadDiffSeconds;
			
			std::stringstream sstr;

			sstr << plotReadNotification->dir;

			for (const auto& relatedPlotList : plotReadNotification->relatedPlotLists)
				sstr << " + " << relatedPlotList.first;

			log_information_if(MinerLogger::plotReader, MinerLogger::hasOutput(DirDone),
				"Dir %s read in %ss (~%s/s)\n"
				"\t%z %s (%s)",
				sstr.str(),
				Poco::DateTimeFormatter::format(span, "%s.%i"),
				memToString(static_cast<Poco::UInt64>(bytesPerSecond), 2),
				plotReadNotification->plotList.size(),
				plotReadNotification->plotList.size() == 1 ? std::string("file") : std::string("files"),
				memToString(totalSizeBytes, 2));
		}

		TAKE_PROBE_DOMAIN("PlotReader.ReadDir", plotReadNotification->dir)
	}
}

void Burst::PlotReadProgress::reset(Poco::UInt64 blockheight, uintmax_t max)
{
	std::lock_guard<std::mutex> guard(mutex_);
	progress_ = 0;
	blockheight_ = blockheight;
	max_ = max;
}

void Burst::PlotReadProgress::add(uintmax_t value, Poco::UInt64 blockheight)
{
	{
		std::lock_guard<std::mutex> guard(mutex_);

		if (blockheight != blockheight_)
			return;

		progress_ += value;
	}

	fireProgressChanged();
}

bool Burst::PlotReadProgress::isReady() const
{
	std::lock_guard<std::mutex> guard(mutex_);
	return progress_ >= max_;
}

uintmax_t Burst::PlotReadProgress::getValue() const
{
	std::lock_guard<std::mutex> guard(mutex_);
	return progress_;
}

float Burst::PlotReadProgress::getProgress() const
{
	std::lock_guard<std::mutex> guard(mutex_);

	if (max_ == 0.f)
		return 0.f;

	return progress_ * 1.f / max_ * 100;
}

void Burst::PlotReadProgress::fireProgressChanged()
{
	auto progress = getProgress();
	progressChanged.notify(this, progress);
}
