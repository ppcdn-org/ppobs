OBS Studio <https://obsproject.com>
===================================

.. image:: https://github.com/obsproject/obs-studio/actions/workflows/push.yaml/badge.svg?branch=master
   :alt: OBS Studio Build Status - GitHub Actions
   :target: https://github.com/obsproject/obs-studio/actions/workflows/push.yaml?query=branch%3Amaster

.. image:: https://badges.crowdin.net/obs-studio/localized.svg
   :alt: OBS Studio Translation Project Progress
   :target: https://crowdin.com/project/obs-studio

.. image:: https://img.shields.io/discord/348973006581923840.svg?label=&logo=discord&logoColor=ffffff&color=7389D8&labelColor=6A7EC2
   :alt: OBS Studio Discord Server
   :target: https://obsproject.com/discord

PPCDN 项目
----------

**PPCDN** 是一个开源社区项目：https://www.pp-cdn.org

PPCDN 为您带来 30%~50% 的带宽 & 成本节约，端到端极致低时延（<500ms）体验：

1. 支持多推流协议（ERTMP/SRT/WHIP）适应不同直播现场网络，并带来极致低时延体验（含播放缓冲时延）：
   P2P 直连时延：<120ms；WHIP 推流时延：<200ms；ERTMP 推流时延：<200ms；SRT 推流时延：<500ms。
2. 兼容多种技术带来 30%~50% 的流量 & 成本下降：支持 HEVC 同播、内嵌 P2P 穿透服务、0 网络转码技术。
3. 支持 ABR 自适应用户端网络，播放更流畅。
4. 提供 API 获取当前播放帧时间戳，可用于游戏结果与音视频流的同步。

PPCDN 跟腾讯云直播相比的优势：时延缩短 1s；0 转码费用；可读取实时时间戳；不同 quality 流切换无黑屏停顿。

- 官方在线 demo：https://pplayer.pp-cdn.org/
- ppobs 下载：https://github.com/ppcdn-org/ppobs/releases/tag/v32.1.2rc008

本仓库是 PPCDN 的 OBS Studio 分支（ppobs），在保留 OBS 采集/合成/编码能力的基础上，增加了：

- WHIP、ERTMP、SRT 推流；WHIP 支持 H264 + HEVC 两条独立会话的多轨同播，SRT 支持单连接多层同播；
- 可选 H5 裁剪流：从主流画面裁剪一个区域，以独立 H264 流推到 ``<streamName>-h5``，适合 H5/小窗播放；
- P2P 推流与 NAT 探测，SRT/ERTMP 可通过伴生输出加入 P2P；
- ppcenter 应用凭证、推流节点选择、NTP 时钟校准、绝对时间戳 SEI；
- 推流降级/丢包控制通道、断线重连、崩溃与异常重连上报；
- Clarity、Temporal Denoise、Beauty、ROI 等画质增强能力。

请先下载 release 中的 ppobs 并完成推流，再打开 demo 观看。

What is OBS Studio?
-------------------

OBS Studio is software designed for capturing, compositing, encoding,
recording, and streaming video content, efficiently.

It's distributed under the GNU General Public License v2 (or any later
version) - see the accompanying COPYING file for more details.

Quick Links
-----------

- Website: https://obsproject.com

- Help/Documentation/Guides: https://github.com/obsproject/obs-studio/wiki

- Forums: https://obsproject.com/forum/

- Build Instructions: https://github.com/obsproject/obs-studio/wiki/Install-Instructions

- Developer/API Documentation: https://obsproject.com/docs

- Donating/backing/sponsoring: https://obsproject.com/contribute

- Bug Tracker: https://github.com/obsproject/obs-studio/issues

Contributing
------------

- If you would like to help fund or sponsor the project, you can do so
  via `Patreon <https://www.patreon.com/obsproject>`_, `OpenCollective
  <https://opencollective.com/obsproject>`_, or `PayPal
  <https://www.paypal.me/obsproject>`_.  See our `contribute page
  <https://obsproject.com/contribute>`_ for more information.

- If you wish to contribute code to the project, please make sure to
  read the coding and commit guidelines:
  https://github.com/obsproject/obs-studio/blob/master/CONTRIBUTING.md
  
- Code for the project follows the code style guidelines, located
  here: https://github.com/obsproject/obs-studio/blob/master/CODESTYLE.md

- Developer/API documentation can be found here:
  https://obsproject.com/docs

- If you wish to contribute translations, do not submit pull requests.
  Instead, please use Crowdin.  For more information read this page:
  https://obsproject.com/wiki/How-To-Contribute-Translations-For-OBS

- Contributors to OBS Studio and related repositories are expected to
  follow our Code of Conduct, which can be read here:
  https://github.com/obsproject/obs-studio/blob/master/COC.rst

- Other ways to contribute are by helping people out with support on
  our forums or in our community chat.  Please limit support to topics
  you fully understand -- bad advice is worse than no advice.  When it
  comes to something that you don't fully know or understand, please
  defer to the official help or official channels.


SAST Tools
----------

`PVS-Studio <https://pvs-studio.com/pvs-studio/?utm_source=website&utm_medium=github&utm_campaign=open_source>`_ - static analyzer for C, C++, C#, and Java code.
