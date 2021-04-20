Advanced Adaptive Micro Player (AAMP)

Source Overview:

aampcli.cpp
- entry point for command line test app

aampgstplayer.cpp
- gstreamer abstraction - allows playback of unencrypted video fragments

base16, _base64
- utility functions

fragmentcollector_hls
- hls playlist parsing and fragment collection

fragmentcollector_mpd
- dash manifest parsing and fragment collection

fragmentcollector_progressive
- raw mp4 playback support

drm
- digital rights management support and plugins

================================================================================================

/opt/aamp.cfg
This optional file supports changes to default logging/behavior and channel remappings to alternate content.
info		enable logging of requested urls
gst		enable gstreamer logging including pipeline dump
progress	enable periodic logging of position
trace		enable dumps of manifests
curl		enable verbose curl logging for manifest/playlist/segment downloads 
curlLicense     enable verbose curl logging for license request (non-secclient)
debug		enable debul level logs
logMetadata	enable timed metadata logging
abr		disable abr mode (defaults on)
default-bitrate	specify initial bitrate while tuning, or target bitrate while abr disabled (defaults to 2500000)
default-bitrate-4k	specify initial bitrate while tuning 4K contents, or target bitrate while abr disabled for 4K contents (defaults to 13000000)
display-offset	default is -1; if set, configures delay before display, gstreamer is-live, and display-offset parameters
throttle	used with restamping (default=1)
flush		if zero, preserve pipeline during channel changes (default=1)
<url1> <url2>	redirects requests to tune to url1 to url2
demux-hls-audio-track=1 // use software demux for audio
demux-hls-video-track=1 // use software demux for video
demux-hls-video-track-tm=1 // use software demux for trickmodes
live-tune-event=0 // send streamplaying when playlist acquired (default)
live-tune-event=1 // send streamplaying when first fragment decrypted
live-tune-event=2 // send streamplaying when first frame visible

vod-tune-event=0 // send streamplaying when playlist acquired (default)
vod-tune-event=1 // send streamplaying when first fragment 
vod-tune-event=2 // send streamplaying when first frame visible

appSrcForProgressivePlayback // Enables appsrc for playing progressive AV type
decoderunavailablestrict     // Reports decoder unavailable GST Warning as aamp error

demuxed-audio-before-video=1 // send audio es before video in case of s/w demux
forceEC3=1 // inserts "-eac3" before .m3u8 in main manifest url. Useful in live environment to test Dolby track.
disableEC3=1 	// removes "-eac3" before .m3u8 in main manifest url. Useful in live environment to disable Dolby track.
		//This flag makes AAC preferred over ATMOS and DD+
		//Default priority of audio selction is ATMOS, DD+ then AAC
disableATMOS=1 //For playback makes DD+ or AAC preferred over ATMOS (EC+3)

live-offset    live offset time in seconds, aamp starts live playback this much time before the live point
cdvrlive-offset    live offset time in seconds for cdvr, aamp starts live playback this much time before the live point
disablePlaylistIndexEvent=1    disables generation of playlist indexed event by AAMP on tune/trickplay/seek
enableSubscribedTags=1    Specifies if subscribedTags[] and timeMetadata events are enabled during HLS parsing, default value: 1 (true)
map-mpd=<domain / host to map> Remap HLS playback url to DASH url for matching domain/host string (.m3u8 to .mpd) 
dash-ignore-base-url-if-slash If present, disables dash BaseUrl value if it is /
fog=0  Implies de-fog' incoming URLS and force direct aamp-only playback
map-m3u8=<domain / host to map> Remap DASH MPD playback url to HLS m3u8 url for matching domain/host string (.mpd to .m3u8) 
min-init-cache	Video duration to be cached before playing in seconds.
networkTimeout=<download time out> Specify download time out in seconds, default is 10 seconds
manifestTimeout=<manifest download time out> Specify manifest download time out in seconds, default is 10 seconds
playlistTimeout=<playlist download time out> Specify playlist download time out in seconds, default is 10 seconds
license-anonymous-request If set, makes PlayReady/WideVine license request without access token
abr-cache-life=<x in sec> lifetime value for abr cache  for network bandwidth calculation(default 5 sec)
abr-cache-length=<x>  length of abr cache for network bandwidth calculation (default 3)
abr-cache-outlier=<x in bytes> Outlier difference which will be ignored from network bandwidth calculation(default 5MB)
abr-nw-consistency=<x> Number of checks before profile incr/decr by 1.This is to avoid frequenct profile switching with network change(default 2)
abr-skip-duration=<x> minimum duration of fragment to be downloaded before triggering abr (default 6 sec).
buffer-health-monitor-delay=<x in sec> Override for buffer health monitor start delay after tune/ seek
buffer-health-monitor-interval=<x in sec> Override for buffer health monitor interval
hls-av-sync-use-start-time=1 Use EXT-X-PROGRAM-DATE to synchronize audio and video playlists. Disabled in default configuration.
playlists-parallel-fetch=1 Fetch audio and video playlists in parallel. Disabled in default configuration.
pre-fetch-iframe-playlist=1 Pre-fetch iframe playlist for VOD. Enabled by default.
license-server-url=<serverUrl> URL to be used for license requests for encrypted(PR/WV) assets.
ck-license-server-url=<serverUrl> URL to be used for Clear Key license requests.
license-retry-wait-time=<x in milli seconds> Wait time before retrying again for DRM license, having value <=0 would disable retry.
vod-trickplay-fps=<x> Specify the framerate for VOD trickplay (defaults to 4)
linear-trickplay-fps=<x> Specify the framerate for Linear trickplay (defaults to 8)
http-proxy=<SCHEME>://<HTTP PROXY IP:HTTP PROXY PORT> Specify the HTTP Proxy with schemes such as http, sock, https etc
http-proxy=<USERNAME:PASSWORD>@<HTTP PROXY IP:HTTP PROXY PORT> Specify the HTTP Proxy with Proxy Authentication Credentials. Make sure to encode special characters if present in username or password (URL Encoding)
mpd-discontinuity-handling=0	Disable discontinuity handling during MPD period transition.
mpd-discontinuity-handling-cdvr=0	Disable discontinuity handling during MPD period transition for cDvr.
force-http Allow forcing of HTTP protocol for HTTPS URLs
internal-retune=0 Disable internal reTune logic on underflows/ pts errors
re-tune-on-buffering-timeout=0 Disable internal re-tune on buffering time-out
gst-buffering-before-play=0 Disable pre buffering logic which ensures minimum buffering is done before pipeline play
audioLatencyLogging  Enable Latency logging for Audio fragment downloads
videoLatencyLogging  Enable Latency logging for Video fragment downloads
iframeLatencyLogging Enable Latency logging for Iframe fragment downloads
pts-error-threshold=<X> aamp maximum number of back-to-back pts errors to be considered for triggering a retune
fragment-cache-length=<X>  aamp fragment cache length (defaults to 3 fragments)
iframe-default-bitrate=<X> specify bitrate threshold for selection of iframe track in non-4K assets( less than or equal to X ). Disabled in default configuration.
iframe-default-bitrate-4k=<X> specify bitrate threshold for selection of iframe track in 4K assets( less than or equal to X ). Disabled in default configuration.
curl-stall-timeout=<X> specify the value in seconds for a CURL download to be deemed as stalled after download freezes, 0 to disable. Disabled by default
curl-download-start-timeout=<X> specify the value in seconds for after which a CURL download is aborted if no data is received after connect, 0 to disable. Disabled by default
playready-output-protection=1  enable HDCP output protection for DASH-PlayReady playback. By default playready-output-protection is disabled.
max-playlist-cache=<X> Max Size of Cache to store the VOD Manifest/playlist . Size in KBytes
wait-time-before-retry-http-5xx-ms=<X> Specify the wait time before retry for 5xx http errors. Default wait time is 1s.
sslverifypeer=<X>	X = 1 to enable TLS certificate verification, X = 0 to disable peer verification.
subtitle-language=<X> ISO 639-1 code of preferred subtitle language
enable_videoend_event=<X>	Enable/Disable Video End event generation; default is 1 (enabled)
dash-max-drm-sessions=<X> Max drm sessions that can be cached by AampDRMSessionManager. Expected value range is 2 to 30 will default to 2 if out of range value is given 
enable_setvideorectangle=0       Disable AAMP to set rectangle property to sink. Default is true(enabled).
discontinuity-timeout=<X>  Value in MS after which AAMP will try recovery for discontinuity stall, after detecting empty buffer, 0 will disable the feature, default 3000
aamp-abr-threshold-size=<X> Specify aamp abr threshold fragment size. Default value is 25000
downloadDelay=<x> Specify the extra ms to be added in each download. Default value is 0. Expected value from 0 to 30000 ms.
harvest-count-limit=<X> Specify the limit of number of files to be harvested
harvest-path=<X> Specify the path where fragments has to be harvested,check folder permissions specifying the path
harvest-config=<X> Specify the value to indicate the type of file to be harvested. It is bitmasking technique, enable more file type by setting more bits
    By default aamp will dump all the type of data, set 0 for desabling harvest
	0x00000001 (1)      - Enable Harvest Video fragments - set 1st bit 
	0x00000002 (2)      - Enable Harvest audio - set 2nd bit 
	0x00000004 (4)      - Enable Harvest subtitle - set 3rd bit 
	0x00000008 (8)      - Enable Harvest auxiliary audio - set 4th bit 
	0x00000010 (16)     - Enable Harvest manifest - set 5th bit 
	0x00000020 (32)     - Enable Harvest license - set 6th bit , TODO: not yet supported license dumping
	0x00000040 (64)     - Enable Harvest iframe - set 7th bit 
	0x00000080 (128)    - Enable Harvest video init fragment - set 8th bit 
	0x00000100 (256)    - Enable Harvest audio init fragment - set 9th bit 
	0x00000200 (512)    - Enable Harvest subtitle init fragment - set 10th bit 
	0x00000400 (1024)   - Enable Harvest auxiliary audio init fragment - set 11th bit 
	0x00000800 (2048)   - Enable Harvest video playlist - set 12th bit 
	0x00001000 (4096)   - Enable Harvest audio playlist - set 13th bit 
	0x00002000 (8192)   - Enable Harvest subtitle playlist - set 14th bit 
	0x00004000 (16384)  - Enable Harvest auxiliary audio playlist - set 15th bit 
	0x00008000 (32768)  - Enable Harvest Iframe playlist - set 16th bit 
	0x00010000 (65536)  - Enable Harvest IFRAME init fragment - set 17th bit  
	example :- if you want harvest only manifest and vide0 fragments , set value like 0x00000001 + 0x00000010 = 0x00000011 = 17
	harvest-config=17
enable-tune-profiling=1 Enable "MicroEvent" tune profiling using - both in splunk (for receiver-integrated aamp) and via console logging
gst-position-query-enable=<X>	if X is 1, then GStreamer position query will be used for progress report events, Enabled by default for non-Intel platforms
disableMidFragmentSeek=1       Disables the Mid-Fragment seek functionality in aamp.

langcodepref=<X>
	0: NO_LANGCODE_PREFERENCE (pass through language codes from manifest - default)
	1: ISO639_PREFER_3_CHAR_BIBLIOGRAPHIC_LANGCODE language codes normalized to 3-character iso639-2 bibliographic encoding(i.e. "ger")
	2: ISO639_PREFER_3_CHAR_TERMINOLOGY_LANGCODE langguage codes normalized to 3-character iso639-2 terminology encoding (i.e. "deu")
	3: ISO639_PREFER_2_CHAR_LANGCODE language codes normalized to 2-character iso639-1 encoding (i.e. "de")

reportbufferevent=<X> Enable/Disable reporting buffer event for buffer underflow, default is 1 (enabled)
propagateUriParameters=0 to disable feature where top-level manifest URI parameters included when downloading fragments.  Default is 1 (enabled).
useWesterosSink=1  Enable player to use westeros sink based video decoding. Default value is false(disabled)
useLinearSimulator Enable linear simulator for testing purpose, simulate VOD asset as a "virtual linear" stream.
useRetuneForUnpairedDiscontinuity=0 To disable unpaired discontinuity retun functionality, by default this is flag enabled.
useRetuneForGSTInternalError=0 To disable retune mitigation for gst pipeline internal data stream error, by default this is flag enabled.
curlHeader=1 enable curl header response logging on curl errors.  Default is false (disabled).
customHeader=<customHeaderString> custom header string data to be appended to curl request
        Note: To add multiple customHeader, add one more line in aamp.cfg and add the data, likewise multiple custom header can be configured.
uriParameter=<uriParameterString> uri parameter data to be appended on download-url during curl request, note that it will be considered the "curlHeader=1" config is set.
fragmentRetryLimit=<X>	Set fragment rampdown/retry limit for video fragment failure, default is 10 (10 retry attempts including rampdown and segment skip).
initRampdownLimit=<X> Maximum number of rampdown/retries for initial playlist retrieval at tune/seek time. Default is 0 (disabled).
minBitrate=<X>		Set minimum bitrate filter for playback profiles, default is 0.
maxBitrate=<X>		Set maximum bitrate filter for playback profiles, default is LONG_MAX.
drmDecryptFailThreshold=<X>	Set retry count on drm decryption failure, default is 10.
segmentInjectFailThreshold=<X>	Set retry count for segment injection discard/failue, default is 10.
initFragmentRetryCount=<X> To set max retry attempts for init frag curl timeout failures, default count is 1 (which internally means 1 download attempt and "1 retry attempt after failure").
use-matching-baseurl=1 Enable host matching while selecting base url, host of main url will be matched with host of base url
disableWifiCurlHeader=1 Disble wifi custom curl header inclusion 
enableSeekableRange=1 Enable seekable range reporting via progress events (startMilliseconds, endMilliseconds)
reportvideopts if present, current video pts is reported via progress events
disableWifiCurlHeader=1 Disble wifi custom curl header inclusion
maxTimeoutForSourceSetup=<X> timeout value in milliseconds to wait for GStreamer appsource setup to complete
useDashParallelFragDownload=1 used to enable/disable dash fragment parallel download logic, by default the value is 1, can be disabled by setting the value to 0
persistBitRateOverSeek=1 used to enable AAMP ABR profile persistence during Seek/Trickplay/Audio switching. By default its disabled and profile switches to default BW
setLicenseCaching=0 used to disable license caching, by default the value is 1 to enable the license caching.
livePauseBehavior=<x> To set live pause behavior (x can be 0-Autoplay immediate, 1-Live immediate, 2-Autoplay defer, 3- Live defer)
disableUnderflow=1 to disable underflow functionality, to enable underflow set the flag to 0. By default the value is 1.
=================================================================================================================
Overriding channels in aamp.cfg
aamp.cfg allows to map channnels to custom urls as follows

*<Token> <Custom url>
This will make aamp tune to the <Custom url> when ever aamp gets tune request to any url with <Token> in it.

Example adding the following in aamp.cfg will make tune to the given url (Spring_4Ktest) on tuning to url with USAHD in it
This can be done for n number of channels.

*USAHD https://dash.akamaized.net/akamai/streamroot/050714/Spring_4Ktest.mpd
*FXHD http://demo.unified-streaming.com/video/tears-of-steel/tears-of-steel-dash-playready.ism/.mpd

=================================================================================================================

To enable Westeros
-------------------

Currently, use of Westeros is default-disabled, and can be enabled via RFC.  To apply, Developers can add below
flag in SetEnv.sh under /opt, then restart the receiver process:

	export AAMP_ENABLE_WESTEROS_SINK=true

Note: Above is now used as a common FLAG by AAMP and Receiver module to configure Westeros direct rendering
instead of going through browser rendering.  This allows for smoother video zoom animations
(Refer DELIA-38429/RDK-26261)

However, note that with this optimization applied, the AAMP Diagnostics overlays cannot be made visible.
As a temporary workaround, the following flag can be used  by developers which will make diagnostic overlay
again visible at expense of zoom smoothness:

	export DISABLE_NONCOMPOSITED_WEBGL_FOR_IPVIDEO=1

=================================================================================================================

CLI-specific commands:
<enter>		dump currently available profiles
help		show usage notes
http://...	tune to specified URL
<number>	tune to specified channel (based on canned aamp channel map)
next        tune to next virtual channel
prev        tune to previous virtual channel
seek <sec>	time-based seek within current content (stub)
ff32		set desired trick speed to 32x
ff16		set desired trick speed to 16x
ff		set desired trick speed to 4x
flush		flush player buffers
stop		stop streaming
status		dump gstreamer state
rect		Set video rectangle. eg. rect 0 0 640 360
zoom <val>	Set video zoom mode. mode "none" if val is zero, else mode "full"
pause       Pause playback
play        Resume playback
rw<val>     Rewind with speed <val>
live        Seek to live point
exit        Gracefully exit application
sap <lang>  Select alternate audio language track.
bps <val>   Set video bitrate in bps

To add channelmap for CLI, enter channel entries in below format in /opt/aampcli.cfg
*<Channel Number> <Channel Name> <Channel URL>

or

To add channelmap for CLI, enter channel entries in below format in /opt/aampcli.csv
<Channel Number>,<Channel Name>,<Channel URL>
================================================================================================
Following line can be added as a header while making CSV with profiler data.

version#4
version,build,tuneStartBaseUTCMS,ManifestDLStartTime,ManifestDLTotalTime,ManifestDLFailCount,VideoPlaylistDLStartTime,VideoPlaylistDLTotalTime,VideoPlaylistDLFailCount,AudioPlaylistDLStartTime,AudioPlaylistDLTotalTime,AudioPlaylistDLFailCount,VideoInitDLStartTime,VideoInitDLTotalTime,VideoInitDLFailCount,AudioInitDLStartTime,AudioInitDLTotalTime,AudioInitDLFailCount,VideoFragmentDLStartTime,VideoFragmentDLTotalTime,VideoFragmentDLFailCount,VideoBitRate,AudioFragmentDLStartTime,AudioFragmentDLTotalTime,AudioFragmentDLFailCount,AudioBitRate,drmLicenseAcqStartTime,drmLicenseAcqTotalTime,drmFailErrorCode,LicenseAcqPreProcessingDuration,LicenseAcqNetworkDuration,LicenseAcqPostProcDuration,VideoFragmentDecryptDuration,AudioFragmentDecryptDuration,gstPlayStartTime,gstFirstFrameTime,contentType,streamType,firstTune,Prebuffered,PreBufferedTime

MicroEvents Information
=====================
Common:
ct = Content Type
it = Initiation Time of Playback in epoch format (on Receiver side)
tt = Total Tune Time/latency
pi = Playback Index
ts = Tune Status
va = Vector of tune Attempts

Individual Tune Attempts:
s = Start Time in epoch format
td = Tune Duration
st = Stream Type
u = URL
r = Result (1:Success, 0:Failure)
v = Vector of Events happened

Events:
i = Id
	0: main manifest download
	1: video playlist download
	2: audio playlist download
	3: subtitle playlist download
	4: video initialization fragment download
	5: audio initialization fragment download
	6: subtitle initialization fragment download
	7: video fragment download
	8: audio fragment download
	9: subtitle fragment download
	10: video decryption
	11: audio decryption
	12: subtitle decryption
	13: license acquisition total
	14: license acquisition pre-processing
	15: license acquisition network
	16: license acquisition post-processing
b = Beginning time of the event, relative to 's'
d = Duration till the completion of event
o = Output of Event (200:Success, Non 200:Error Code)


VideoEnd (Session Statistics) Event Information
==========================
vr = version of video end event (currently "1.0")
tt = time to reach top profile first time after tune. Provided initial tune bandwidth is not a top bandwidth
ta = time at top profile. This includes all the fragments which are downloaded/injected at top profile for total duration of playback. 
d = duration - estimate of total playback duration.  Note that this is based on fragments downloaded/injected - user may interrupt buffered playback with seek/stop, causing estimates to skew higher in edge cases.
dn = Download step-downs due to bad Network bandwidth
de = Download step-downs due to Error handling ramp-down/retry logic
w = Display Width :  value > 0 = Valid Width.. value -1 means HDMI display resolution could NOT be read successfully. Only for HDMI Display else wont be available.
h = Display Height : value > 0 = Valid Height,  value -1 means HDMI display resolution could NOT be read successfully. Only for HDMI Display else wont be available.
t = indicates that FOG time shift buffer (TSB) was used for playback
m =  main manifest
v = video Profile
i = Iframe Profile
a1 = audio track 1
a2 = audio track 2
a3 = audio track 3
...
u = Unknown Profile or track type

l = supported languages
p = profile-specific metrics encapsulation
w = profile frame width
h = profile frame height
ls = license statistics

ms = manifest statistics
fs = fragment statistics

r = total license rotations / stream switches
e = encrypted to clear switches
c = clear to encrypted switches

4 = HTTP-4XX error count
5 = HTTP-5XX error count
t = CURL timeout error count
c = CURL error count (other)
s = successful download count

u = URL of most recent (last) failed download
n = normal fragment statistics
i = "init" fragment statistics (used in case of DASH and fragmented mp4)
