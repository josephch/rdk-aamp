window.onload = function() {
    var ROWHEIGHT = 24;
    var BITRATE_MARGIN = 112;
    var TIMELINE_MARGIN = 80;
    var BITRATE_SCALE = (600 / 10000000) * 3;
    var data;
    var sessionData = [];
    var sessionClicked = false;
    var sessionClickedID = 0;

    function myclickhandler(event) {
        var x = event.pageX;
        var y = event.pageY - ROWHEIGHT;
        for (var i = 0; i < data.length; i++) {
            if (x >= data[i].x && x < data[i].x + data[i].w &&
                y >= data[i].y && y < data[i].y + ROWHEIGHT) {
                alert(mediaTypes[data[i].type] + " " + data[i].error + "\n" + data[i].url);
                break;
            }
        }
    }

    var canvas = document.getElementById("myCanvas");
    canvas.onclick = myclickhandler;

    var timestamp_min;
    var timestamp_max;
    var bitrate_max;
    var allbitrates;

    function time2x(t) {
        return BITRATE_MARGIN + 16 + (t - timestamp_min) * 0.1;
    }

    function bitrate2y(bitrate) {
        for( var i=0; i<allbitrates.length; i++ )
        {
            if( allbitrates[i] == bitrate ) return i*ROWHEIGHT*2 + 64;
        }
        return 0;
    }
                         
    function AddBitrate( newBitrate )
    {
        for( var i=0; i<allbitrates.length; i++ )
        {
            if( allbitrates[i]==newBitrate ) return;
        }
        allbitrates.push( newBitrate );
    }
    
    var topMargin = 24;

	
	function MapMediaColor(mediaType)
 { // first color for box interior; second color for outline
	 switch( mediaType )
	 {
		 case eMEDIATYPE_MANIFEST:
			 return ['#00cccc','#006666']; // cyan;
		 case eMEDIATYPE_PLAYLIST_VIDEO:
			 return ['#00cc00','#006600']; // dark green
		 case eMEDIATYPE_INIT_VIDEO:
			 return ['#3fbb3f','#3f7f3f']; // medium green
		 case eMEDIATYPE_VIDEO:
			 return ['#ccffcc','#667f66']; // light green
		 case eMEDIATYPE_PLAYLIST_IFRAME:
			 return ['#00cc00','#006600']; // dark green
		 case eMEDIATYPE_INIT_IFRAME:
			 return ['#7fff7f','#3f7f3f']; // medium green
		 case eMEDIATYPE_IFRAME:
			 return ['#ccffcc','#667f66']; // light green
		 case eMEDIATYPE_PLAYLIST_AUDIO:
			 return ['#0000cc','#000066']; // dark blue
		 case eMEDIATYPE_INIT_AUDIO:
			 return ['#7f7fff','#3f3f7f']; // medium blue
		 case eMEDIATYPE_AUDIO:
			 return ['#ccccff','#66667f']; // light blue
		 case eMEDIATYPE_PLAYLIST_SUBTITLE:
			 return ['#cccc00','#666600']; // dark yellow
		 case eMEDIATYPE_INIT_SUBTITLE:
			 return ['#cccc3f','#7f7f3f']; // medium yellow
		 case eMEDIATYPE_SUBTITLE:
			 return ['#ffffcc','#7f7f66']; // light yellow
		 case eMEDIATYPE_LICENSE:
			 return ['#ff7fff','#7f3f7f']; // medium magenta
		 case -eMEDIATYPE_LICENSE: // pre/post overhead
			 return ['#ffccff','#7f667f'];
		 default: // error
			 return ['#ff2020','#7f3f3f'];
	 }
 }
	
    function myLoadHandler(e) {
        var iframe_seqstart = null;
        var av_seqstart = null;
        var bandwidth_samples = [];
        timestamp_min = null;
        timestamp_max = null;
        bitrate_max = null;
        allbitrates = ["audio","subtitle","manifest"];
        var marker = [];

        if (!sessionClicked) {
            // parse data
            var raw = e.target.result;

			var sessions;
            // cut out single sessions from a bigger log
			if( raw.indexOf("aamp_tune:")>=0 )
			{
				sessions = raw.split("aamp_tune:");
			}
			else
			{
				sessions = raw.split("[processTSBRequest][INFO]new");
			}
			
			sessions.shift(); // to remove first null match

            var currentSession = document.getElementById("session");
            while (currentSession.firstChild) {
                currentSession.removeChild(currentSession.firstChild);
            }
            for (iter = 1; iter <= sessions.length; iter++) {
                var option = document.createElement("option");
                option.text = iter;
                option.value = iter;
                var sessionDataItem = sessions[iter - 1];
                sessionData.push(sessionDataItem);
                currentSession.add(option);
            }
            var samples = sessions[0].split("\n");
        } else {
            var samples = sessionData[sessionClickedID - 1].split("\n");
        }


        data = [];
                         
        for (var i = 0; i < samples.length; i++) {
            var line = samples[i];
			var httpRequestEnd = ParseHttpRequestEnd(line);
			if( httpRequestEnd )
			{
				var obj = {};
				obj.error = mapError(httpRequestEnd.responseCode);
				obj.durationms = 1000*parseFloat(httpRequestEnd.total);
				obj.type = parseInt(httpRequestEnd.type);
				obj.bytes = parseInt(httpRequestEnd.dlSz);
				obj.url = httpRequestEnd.url;
				var doneUtc = ParseReceiverLogTimestamp(line);
				obj.utcstart = doneUtc-obj.durationms;
				if (timestamp_min == null || obj.utcstart < timestamp_min) timestamp_min = obj.utcstart;
				if (timestamp_max == null || doneUtc > timestamp_max) timestamp_max = doneUtc;
				if( obj.type==eMEDIATYPE_PLAYLIST_VIDEO || obj.type==eMEDIATYPE_VIDEO || obj.type == eMEDIATYPE_INIT_VIDEO )
				{
					obj.bitrate = httpRequestEnd.br;
					AddBitrate( parseInt(obj.bitrate) );
                    console.log(httpRequestEnd.br)
				}
				else if( obj.type == eMEDIATYPE_AUDIO || obj.type == eMEDIATYPE_INIT_AUDIO )
				{
					obj.bitrate = "audio";
				}
				else if( obj.type == eMEDIATYPE_SUBTITLE || obj.type == eMEDIATYPE_INIT_SUBTITLE )
				{
					obj.bitrate = "subtitle";
				}
				else if( obj.type == eMEDIATYPE_MANIFEST )
				{
					obj.bitrate = "manifest";
				}
				data.push(obj);
			}
        } // next line
        allbitrates.sort( function(a,b){return b-a;} );
                         
        var canvas = document.getElementById("myCanvas");
        canvas.width = Math.min(10000, time2x(timestamp_max) + 16); // cap max width to avoid canvas limitation
        canvas.height = allbitrates.length*ROWHEIGHT*2 + 480;

        var ctx = canvas.getContext("2d");
        ctx.clearRect(0, 0, canvas.width, canvas.height);
        ctx.font = "18px Arial";

        // timeline backdrop
        ctx.textAlign = "center";
        var y0 = bitrate2y(0)+ROWHEIGHT;
        var y1 = bitrate2y(bitrate_max)-ROWHEIGHT;
        var shade = true;
        for (var t0 = timestamp_min; t0 < timestamp_max; t0 += 1000) {
            var x0 = time2x(t0);
            var x1 = time2x(t0 + 1000);
            if (shade) { // light grey band for every other second
                ctx.fillStyle = '#f3f3f3';
                ctx.fillRect(x0, y0, x1 - x0, y1 - y0);
            }
            shade = !shade;

            ctx.strokeStyle = '#dddddd';
            ctx.strokeRect(x0, topMargin, 1, y1-topMargin);

            ctx.fillStyle = '#000000';
            ctx.fillText((t0 - timestamp_min) / 1000 + "s", x0, topMargin );
        }

        // draw y-axis labels
        ctx.textAlign = "right";
        ctx.strokeStyle = '#dddddd';
        for (var i = 0; i < allbitrates.length; i++) {
            var bitrate = allbitrates[i];
            var y = bitrate2y(bitrate);
            ctx.strokeRect(BITRATE_MARGIN + 2, y, canvas.width, 1);

            ctx.fillStyle = '#000000';
            var label = bitrate;//(bitrate==0)?"audio/other":bitrate;
            ctx.fillText(label, BITRATE_MARGIN, y + ROWHEIGHT / 2 - 3);
        }
        
        ctx.textAlign = "center";
        for( var i=0; i<marker.length; i++ )
        {
            var x = time2x(marker[i][0]);
            var label = marker[i][1];
            var y = (i%8)*24 + topMargin+y0+64;

            ctx.fillStyle = '#cc0000';
            ctx.fillRect(x, topMargin, 1, y-topMargin );
                         
            ctx.fillStyle = '#000000';
            ctx.fillText(label, x, y+16 );
        }

        ctx.textAlign = "left";

        // compute default positions of bars
        for (var i = 0; i < data.length; i++) {
            var t0 = data[i].utcstart;
            var t1 = data[i].durationms + t0;
            data[i].x = time2x(t0);
            data[i].w = time2x(t1) - data[i].x;
            var bitrate = data[i].bitrate;
            data[i].y = bitrate2y(bitrate);
        }

        // adjust bar placement to avoid overlap w/ parallel downloads
        for (;;) {
            var bump = false;
            var pad = 0; // +16 used to include labels poking out past bars as consideration for overlap
            for (var i = 0; i < data.length; i++) {
                for (var j = i + 1; j < data.length; j++) {
                    if (
                        data[i].x + data[i].w + pad > data[j].x &&
                        data[j].x + data[j].w + pad > data[i].x &&
                        data[i].y + ROWHEIGHT > data[j].y &&
                        data[j].y + ROWHEIGHT > data[i].y) {
                        data[j].y += ROWHEIGHT;
                        bump = true;
                    }
                }
            }
            if (!bump) {
                break;
            }
        }

        for (var i = 0; i < data.length; i++) {
            // map colors based on download type and success/failure
            if (data[i].error != "HTTP200(OK)" ) {
                ctx.fillStyle = '#ff0000';
				ctx.strokeStyle = '#3f0000';
            }
            else
            {
				var coloring = MapMediaColor(data[i].type);
				ctx.fillStyle = coloring[0];
				ctx.strokeStyle = coloring[1];
			}
			
            ctx.fillRect(data[i].x, data[i].y - ROWHEIGHT / 2, data[i].w, ROWHEIGHT - 1);
//            ctx.strokeStyle = '#999999';
			ctx.strokeStyle = '#000000';
            ctx.strokeRect(data[i].x, data[i].y - ROWHEIGHT / 2, data[i].w, ROWHEIGHT - 1);
        }
    }

    function handleFileSelect(evt) {
        sessionData = [];
        sessionClickedID = 0;
        sessionClicked = false;
        var files = evt.target.files;
        for (var fileIndex = 0; fileIndex < files.length; fileIndex++) {
            var f = files[fileIndex];
            if (f.type = "text/plain") {
                var reader = new FileReader();
                reader.onload = myLoadHandler;
                reader.readAsText(f);
            }
        }
    }

    // For a new Tune Session
    function newSession(evt) {
        var currentSession = document.getElementById("session");
        sessionClickedID = currentSession.options[currentSession.selectedIndex].value;
        sessionClicked = true;
        myLoadHandler();
    }

    // For a new checkbox change request
    function newCheckBoxChange(evt) {
        if(sessionClicked) {
            myLoadHandler();
        } else {
            var event = new Event('change');
            // Dispatch the change event
            document.getElementById('files').dispatchEvent(event);
        }
        
    }

    document.getElementById('files').addEventListener('change', handleFileSelect, false);
    document.getElementById('session').addEventListener('change', newSession, false);
}
