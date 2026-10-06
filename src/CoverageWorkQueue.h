// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>

namespace CoverageWorkQueue
{
	template <uint32_t N>
	class Grid
	{
		static_assert(N > 0 && (N & (N-1)) == 0);
		static constexpr uint32_t total = N*N;
		std::array<uint32_t,total> entries{};
		std::array<bool,total> queued{};
		uint32_t head{}, tail{}, count{};
		int32_t centreX{}, centreY{};
		bool valid{};
	public:
		static uint32_t Index(int32_t x,int32_t y) { return (static_cast<uint32_t>(y)&(N-1))*N+(static_cast<uint32_t>(x)&(N-1)); }
		uint32_t Count() const { return count; }
		void Reset() { queued.fill(false); head=tail=count=0; valid=false; }
		void Push(uint32_t index) {
			if (queued[index]) { return; }
			queued[index]=true; entries[tail]=index; tail=(tail+1)%total; ++count;
		}
		uint32_t Pop() {
			const auto index=entries[head];head=(head+1)%total;--count;queued[index]=false;return index;
		}
		template<class Entering>
		void Move(int32_t x,int32_t y,Entering&& entering) {
			if (valid && x==centreX && y==centreY) { return; }
			const int64_t dx=static_cast<int64_t>(x)-centreX,dy=static_cast<int64_t>(y)-centreY;
			const int32_t bx=x-static_cast<int32_t>(N/2),by=y-static_cast<int32_t>(N/2);
			const auto add=[&](int32_t cx,int32_t cy) { const auto i=Index(cx,cy); entering(i);Push(i); };
			if (!valid || std::abs(dx)>=N || std::abs(dy)>=N) {
				for(uint32_t cy=0;cy<N;++cy) { for(uint32_t cx=0;cx<N;++cx) { add(bx+cx,by+cy); } }
			} else {
				const int32_t oldX=centreX-static_cast<int32_t>(N/2),oldY=centreY-static_cast<int32_t>(N/2);
				const int32_t startX=dx>0 ? oldX+N : bx,endX=dx>0 ? bx+N : oldX;
				for(int32_t cx=startX;cx<endX;++cx) { for(uint32_t cy=0;cy<N;++cy) { add(cx,by+cy); } }
				const int32_t startY=dy>0 ? oldY+N : by,endY=dy>0 ? by+N : oldY;
				for(int32_t cy=startY;cy<endY;++cy) { for(uint32_t cx=0;cx<N;++cx) { add(bx+cx,cy); } }
			}
			centreX=x;centreY=y;valid=true;
		}
	};
}
