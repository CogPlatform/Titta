% this demo code is part of Titta, a toolbox providing convenient access to
% eye tracking functionality using Tobii eye trackers
%
% Titta can be found at https://github.com/dcnieho/Titta. Check there for
% the latest version.
% When using Titta, please cite the following paper:
%
% Niehorster, D.C., Andersson, R. & Nystrom, M., (2020). Titta: A toolbox
% for creating Psychtoolbox and Psychopy experiments with Tobii eye
% trackers. Behavior Research Methods.
% doi: https://doi.org/10.3758/s13428-020-01358-8

clear all
sca


DEBUGlevel              = 0;
bgClr                   = 127;
useAnimatedCalibration  = true;
scr                     = max(Screen('Screens'));
% task parameters
wallyPos = [965, 74];           % Pixel position of Wally in image
wallyImgFile = 'wally_search.png';
wallyFaceImgFile = 'wally_face.jpg';
maxSearchTime = 20;             % seconds
drawOwnGaze = true;             % Draw gaze marker for own gaze (true) or only for others?
filterGaze = true;

% ensure Titta, TittaLSL and the LSL libraries are on path
home = cd;
cd ..; cd ..;
addTittaToPath;
cd(home);
addpath(genpath('liblsl_Matlab'));
% load LSL
lslLib = lsl_loadlib();
fprintf('Using LSL v%d\n',lsl_library_version(lslLib));


dotColors = [240,163,255;0,117,220;153,63,0;76,0,92;25,25,25;0,92,49;43,206,72;255,204,153;128,128,128;148,255,181;143,124,0;157,204,0;194,0,136;0,51,128;255,164,5;255,168,187;66,102,0;255,0,16;94,241,242;0,153,143;224,255,102;116,10,255;153,0,0;255,255,128;255,255,0;255,80,5];
try
    % get setup struct (can edit that of course):
    settings = Titta.getDefaults('Tobii Pro Spectrum');
    % request some debug output to command window, can skip for normal use
    settings.debugMode      = true;
    % calibration display: custom calibration drawer
    calViz                      = AnimatedCalibrationDisplay();
    settings.cal.drawFunction   = @calViz.doDraw;
    
    % init
    EThndl          = Titta(settings);
    EThndl.init();

    % setup connection with master
    hostname = string(java.net.InetAddress.getLocalHost().getHostName());
    % Find master and open a communication channel with it so that
    % commands from the master can be received
    streams = lsl_resolve_byprop(lslLib, 'type', 'Wally_master', 1, 32000000);
    if length(streams)>1
        error('More than one Wally_master found on the network, can''t continue')
    end
    from_master = lsl_inlet(streams{1});
    
    % Open a communication channel to send messages to the master
    info = lsl_streaminfo(lslLib, 'Wally_finder', 'Wally_client', 1, 0, 'cf_string', sprintf('%s_%s',hostname,EThndl.serialNumber));
    to_master = lsl_outlet(info,1);

    % ensure we're properly connected
    warm_up_bidirectional_comms(to_master, from_master);

    % tell master about the connected eye tracker
    wait_for_message('get_eye_tracker', from_master);
    msg = sprintf('eye_tracker,%s,%s',EThndl.serialNumber,EThndl.address);
    to_master.push_sample({msg});
    
    if DEBUGlevel>1
        % make screen partially transparent on OSX and windows vista or
        % higher, so we can debug.
        PsychDebugWindowConfiguration;
    end
    if DEBUGlevel
        % Be pretty verbose about information and hints to optimize your code and system.
        Screen('Preference', 'Verbosity', 4);
    else
        % Only output critical errors and warnings.
        Screen('Preference', 'Verbosity', 2);
    end
    Screen('Preference', 'SyncTestSettings', 0.002);    % the systems are a little noisy, give the test a little more leeway
    [wpnt,winRect] = PsychImaging('OpenWindow', scr, bgClr, [], [], [], [], 4);
    hz=Screen('NominalFrameRate', wpnt);
    Priority(1);
    Screen('BlendFunction', wpnt, GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    Screen('Preference', 'TextAlphaBlending', 1);
    Screen('Preference', 'TextAntiAliasing', 2);
    % This preference setting selects the high quality text renderer on
    % each operating system: It is not really needed, as the high quality
    % renderer is the default on all operating systems, so this is more of
    % a "better safe than sorry" setting.
    Screen('Preference', 'TextRenderer', 1);
    KbName('UnifyKeyNames');    % for correct operation of the setup/calibration interface, calling this is required
    
    % prep stimuli (get Wally)
    wallies = loadStimuliFromFolder(cd,{'wally_face.jpg','wally_search.png'},wpnt,winRect(3:4));
    
    % do calibration
    try
        ListenChar(-1);
    catch ME
        % old PTBs don't have mode -1, use 2 instead which also supresses
        % keypresses from leaking through to matlab
        ListenChar(2);
    end
    tobii.calVal        = EThndl.calibrate(wpnt);
    ListenChar(0);

    % send calibration results to master
    if ~isnan(tobii.calVal.selectedCal(1))
        iVal= find(cellfun(@(x) x.status, tobii.calVal.attempt{tobii.calVal.selectedCal(1)}.val)==1,1,'last');
        val = tobii.calVal.attempt{tobii.calVal.selectedCal(1)}.val{iVal};
    else
        val = [];
    end
    if isfield(val,'quality')
        dev_L = val.acc1D(1);
        dev_R = val.acc1D(2);
    else
        dev_L = nan;
        dev_R = nan;
    end
    msg = sprintf('calibration_done,%.2f,%.2f', dev_L, dev_R);
    to_master.push_sample({msg});
    
    % wait for master to tell us which clients to connect to
    connect_to = wait_for_message(sprintf('connect_to,%s',hostname), from_master, '', true);
    hosts_to_connect_to = cellfun(@(x) x{1},connect_to,'UniformOutput',false);
    
    % start the eye tracker
    EThndl.buffer.start('gaze','','',true); % start eye tracker and wait for gaze to be picked up
    
    % create a TittaLSLPy sender that makes this eye tracker's gaze data stream available on the network
    sender = TittaLSL.Sender(EThndl.address);
    sender.start('gaze');
    
    % Start receiving et data with LSL (find started remote streams)
    receivers   = cell(0,2);
    t0          = GetSecs();
    while true
        remote_streams = TittaLSL.Receiver.GetStreams("gaze");
        for r=1:length(remote_streams)
            h = remote_streams(r).hostname;
            if any(strcmp(h,receivers(:,1))) || ~any(strcmp(h,hosts_to_connect_to))
                continue
            end
    
            receiver = TittaLSL.Receiver(remote_streams(r).source_id);
            receiver.start();
            receivers(end+1,:) = {h, receiver};
        end
        
        if size(receivers,1)==length(hosts_to_connect_to)
            break
        end
    
        WaitSecs(0.05);
    end
    
    % prep filters
    if filterGaze
        filters = {'local', OlssonFilter()};
        for r=1:size(receivers,1)
            filters(end+1,:) = {receivers{r,1}, OlssonFilter()};
        end
    end

    % prep gaze timestamps
    last_ts = {'local', 0.};
    for r=1:size(receivers,1)
        last_ts(end+1,:) = {receivers{r,1}, 0.};
    end

    % Show wally and wait for command to start exp
    [w,h] = RectSize(winRect);
    wallyRect = CenterRectOnPoint(wallies(1).scrRect,w/2,h/4);
    Screen('DrawTexture',wpnt,wallies(1).tex,[],wallyRect);
    DrawFormattedText(wpnt,sprintf('Press the spacebar as soon as you have found Wally\n\nPlease wait for the experiment to start.'),'center','center',255);
    Screen('Flip',wpnt);

    % Wait for start command
    wait_for_message('start_exp', from_master);


    % done, clean up
    for r=1:size(receivers,1)
        receivers{r,2}.stop();
    end
    sender.stop('gaze')
    EThndl.buffer.stop('gaze');
    
    % save data to mat file, adding info about the experiment
    dat = EThndl.collectSessionData();
    dat.expt.resolution = winRect(3:4);
    dat.expt.stim       = wallies;
    % also add all the remote data
    dat.remoteGaze = cell(0,2);
    for r=1:size(receivers,1)
        dat.remoteGaze(end+1,:) = {receivers{r,1}, receivers{r,2}.consumeN()};
    end
    % save
    EThndl.saveData(dat, fullfile(cd,'t'), true);
    
    % shut down
    EThndl.deInit();
catch me
    sca
    ListenChar(0);
    rethrow(me)
end
sca
