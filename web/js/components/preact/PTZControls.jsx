/**
 * PTZ Controls Component
 * Pan-Tilt-Zoom controls for PTZ-enabled cameras
 */

import { useState, useEffect, useCallback, useRef } from 'preact/hooks';
import { useI18n } from '../../i18n.js';
import { showSuccessToast, showErrorToast } from './ToastContainer.jsx';
import {
  getPtzCapabilities, getPtzPresets, savePtzPreset, setPtzHome,
  ptzMove, ptzStop, ptzHome, ptzGotoPreset, deletePtzPreset
} from './ptzActions.js';

/**
 * Direction button component dengan ikon SVG modern & efek visual
 */
function DirectionButton({
  direction,
  onMouseDown,
  onMouseUp,
  onMouseLeave,
  disabled
}) {
  const icons = {
    up: (
      <svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="2.5" strokeLinecap="round" strokeLinejoin="round">
        <path d="M18 15l-6-6-6 6" />
      </svg>
    ),
    down: (
      <svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="2.5" strokeLinecap="round" strokeLinejoin="round">
        <path d="M6 9l6 6 6-6" />
      </svg>
    ),
    left: (
      <svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="2.5" strokeLinecap="round" strokeLinejoin="round">
        <path d="M15 18l-6-6 6-6" />
      </svg>
    ),
    right: (
      <svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="2.5" strokeLinecap="round" strokeLinejoin="round">
        <path d="M9 18l6-6-6-6" />
      </svg>
    ),
    'zoom-in': (
      <svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="2" strokeLinecap="round" strokeLinejoin="round">
        <circle cx="11" cy="11" r="8" />
        <line x1="21" y1="21" x2="16.65" y2="16.65" />
        <line x1="11" y1="8" x2="11" y2="14" />
        <line x1="8" y1="11" x2="14" y2="11" />
      </svg>
    ),
    'zoom-out': (
      <svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="2" strokeLinecap="round" strokeLinejoin="round">
        <circle cx="11" cy="11" r="8" />
        <line x1="21" y1="21" x2="16.65" y2="16.65" />
        <line x1="8" y1="11" x2="14" y2="11" />
      </svg>
    )
  };

  return (
    <button
      className={`ptz-btn ptz-btn-${direction}`}
      onMouseDown={onMouseDown}
      onMouseUp={onMouseUp}
      onMouseLeave={onMouseLeave}
      onTouchStart={onMouseDown}
      onTouchEnd={onMouseUp}
      onTouchCancel={onMouseUp}
      disabled={disabled}
      style={{
        width: '32px',
        height: '32px',
        borderRadius: '50%',
        border: '1px solid rgba(255, 255, 255, 0.2)',
        backgroundColor: 'rgba(255, 255, 255, 0.08)',
        color: '#ffffff',
        cursor: disabled ? 'not-allowed' : 'pointer',
        display: 'flex',
        alignItems: 'center',
        justifyContent: 'center',
        transition: 'all 0.15s ease-in-out',
        opacity: disabled ? 0.35 : 1,
        userSelect: 'none',
        boxShadow: '0 2px 4px rgba(0, 0, 0, 0.2)',
        backdropFilter: 'blur(4px)'
      }}
    >
      {icons[direction]}
    </button>
  );
}

export function PTZControls({
  stream,
  isVisible = true,
  onClose
}) {
  const { t } = useI18n();

  const [speed, setSpeed] = useState(0.5);
  const [presets, setPresets] = useState([]);
  const [selectedPreset, setSelectedPreset] = useState('');
  const [isMoving, setIsMoving] = useState(false);
  const [error, setError] = useState(null);
  const [capabilities, setCapabilities] = useState(null);
  const [canManage, setCanManage] = useState(true);
  const [presetName, setPresetName] = useState('');
  const [showPresetForm, setShowPresetForm] = useState(false);
  const [confirmSetHome, setConfirmSetHome] = useState(false);
  const [saving, setSaving] = useState(false);

  const DEFAULT_POSITION = { x: 10, y: 10 };

  // =========================
  // DRAG STATE & FUNCTIONS
  // =========================
  const panelRef = useRef(null);
  const [position, setPosition] = useState(DEFAULT_POSITION);

  const dragData = useRef({
    dragging: false,
    startX: 0,
    startY: 0,
    initialX: 10,
    initialY: 10
  });

  const handleWheel = useCallback((e) => {
    e.stopPropagation();
    if (e.cancelable) {
      e.preventDefault();
    }
  }, []);

  const clampPosition = useCallback((x, y) => {
    const panel = panelRef.current;
    if (!panel) return { x, y };

    const parent = panel.parentElement;
    if (!parent) return { x, y };

    const parentRect = parent.getBoundingClientRect();
    const panelRect = panel.getBoundingClientRect();

    const maxX = parentRect.width - panelRect.width;
    const maxY = parentRect.height - panelRect.height;

    return {
      x: Math.max(0, Math.min(x, maxX)),
      y: Math.max(0, Math.min(y, maxY))
    };
  }, []);

  useEffect(() => {
    const handleFullscreenChange = () => {
      setPosition(DEFAULT_POSITION);
    };

    document.addEventListener('fullscreenchange', handleFullscreenChange);
    return () => {
      document.removeEventListener('fullscreenchange', handleFullscreenChange);
    };
  }, []);

  const startDrag = useCallback((e) => {
    const panel = panelRef.current;
    if (!panel) return;

    const clientX = e.touches ? e.touches[0].clientX : e.clientX;
    const clientY = e.touches ? e.touches[0].clientY : e.clientY;

    dragData.current = {
      dragging: true,
      startX: clientX,
      startY: clientY,
      initialX: position.x,
      initialY: position.y
    };

    if (e.cancelable) {
      e.preventDefault();
    }
  }, [position]);

  useEffect(() => {
    const onDrag = (e) => {
      if (!dragData.current.dragging) return;

      const clientX = e.touches ? e.touches[0].clientX : e.clientX;
      const clientY = e.touches ? e.touches[0].clientY : e.clientY;

      const deltaX = clientX - dragData.current.startX;
      const deltaY = dragData.current.startY - clientY;

      const newX = dragData.current.initialX + deltaX;
      const newY = dragData.current.initialY + deltaY;

      const clamped = clampPosition(newX, newY);
      setPosition(clamped);
    };

    const stopDrag = () => {
      dragData.current.dragging = false;
    };

    window.addEventListener('mousemove', onDrag);
    window.addEventListener('mouseup', stopDrag);
    window.addEventListener('touchmove', onDrag, { passive: false });
    window.addEventListener('touchend', stopDrag);

    return () => {
      window.removeEventListener('mousemove', onDrag);
      window.removeEventListener('mouseup', stopDrag);
      window.removeEventListener('touchmove', onDrag);
      window.removeEventListener('touchend', stopDrag);
    };
  }, [clampPosition]);

  // =========================
  // LOAD PRESETS & CAPABILITIES
  // =========================
  useEffect(() => {
    if (!stream?.name || !stream?.ptz_enabled) return;
    let active = true;
    setCapabilities(null);
    setCanManage(true);
    setPresets([]);
    setSelectedPreset('');
    setPresetName('');
    setShowPresetForm(false);
    setConfirmSetHome(false);
    setPosition(DEFAULT_POSITION);
    getPtzCapabilities(stream.name)
      .then(data => { if (active) setCapabilities(data); })
      .catch(err => console.error('Failed to get PTZ capabilities:', err));
    getPtzPresets(stream.name)
      .then(data => { if (active) setPresets(data.presets || []); })
      .catch(err => console.error('Failed to get PTZ presets:', err));
    return () => { active = false; };
  }, [stream?.name, stream?.ptz_enabled]);

  const handleActionFailure = useCallback((err, message) => {
    if (err.status === 403) {
      setCanManage(false);
      setShowPresetForm(false);
      setConfirmSetHome(false);
    }
    showErrorToast(message);
    console.error(message, err);
  }, []);

  const handleSavePreset = useCallback(async (event) => {
    event.preventDefault();
    const name = presetName.trim();
    if (!stream?.name || !name || saving) return;
    setSaving(true);
    try {
      await savePtzPreset(stream.name, name, setPresets);
      setShowPresetForm(false);
      setPresetName('');
      showSuccessToast(t('live.ptzPresetSaved'));
    } catch (err) {
      handleActionFailure(err, t(err.stage === 'refresh'
        ? 'live.ptzPresetRefreshFailed' : 'live.ptzPresetSaveFailed'));
    } finally {
      setSaving(false);
    }
  }, [stream?.name, presetName, saving, t, handleActionFailure]);

  const handleDeletePreset = useCallback(async () => {
    if (!stream?.name || !selectedPreset || saving) return;
    setSaving(true);
    try {
      await deletePtzPreset(stream.name, selectedPreset);
      setPresets(prev => prev.filter(p => p.token !== selectedPreset));
      setSelectedPreset('');
      showSuccessToast(t('live.ptzPresetDeleted') || 'Preset deleted');
    } catch (err) {
      handleActionFailure(err, t('live.ptzPresetDeleteFailed') || 'Failed to delete preset');
    } finally {
      setSaving(false);
    }
  }, [stream?.name, selectedPreset, saving, t, handleActionFailure]);

  const handleSetHome = useCallback(async () => {
    if (!stream?.name || saving) return;
    setSaving(true);
    try {
      await setPtzHome(stream.name);
      setConfirmSetHome(false);
      showSuccessToast(t('live.ptzHomeSet'));
    } catch (err) {
      handleActionFailure(err, t('live.ptzSetHomeFailed'));
    } finally {
      setSaving(false);
    }
  }, [stream?.name, saving, t, handleActionFailure]);

  const handleMoveStart = useCallback((pan, tilt, zoom) => {
    if (!stream?.name) return;
    setIsMoving(true);
    setError(null);
    
    ptzMove(stream.name, pan * speed, tilt * speed, zoom * speed)
      .catch(err => {
        setIsMoving(false);
        setError(t('live.ptzMoveFailed'));
        console.error('PTZ move error:', err);
      });
  }, [stream?.name, speed, t]);

  const handleMoveStop = useCallback(() => {
    if (!stream?.name || !isMoving) return;
    setIsMoving(false);
    
    ptzStop(stream.name)
      .catch(err => console.error('PTZ stop error:', err));
  }, [stream?.name, isMoving]);

  const handleHome = useCallback(() => {
    if (!stream?.name) return;
    setError(null);
    
    ptzHome(stream.name)
      .catch(err => {
        setError(t('live.ptzHomeFailed'));
        console.error('PTZ home error:', err);
      });
  }, [stream?.name, t]);

  const handlePreset = useCallback((token) => {
    if (!stream?.name) return;
    setError(null);
    
    ptzGotoPreset(stream.name, token)
      .catch(err => {
        setError(t('live.ptzPresetFailed'));
        console.error('PTZ preset error:', err);
      });
  }, [stream?.name, t]);

  if (!isVisible || !stream?.ptz_enabled) {
    return null;
  }

  return (
    <div
      ref={panelRef}
      className="ptz-controls"
      onWheel={handleWheel}
      style={{
        position: 'absolute',
        left: `${position.x}px`,
        bottom: `${position.y}px`,
        backgroundColor: 'rgba(0, 0, 0, 0.38)',
        backdropFilter: 'blur(6px)',
        WebkitBackdropFilter: 'blur(6px)',
        borderRadius: '16px',
        border: '1px solid rgba(255, 255, 255, 0.25)',
        boxShadow: '0 8px 32px 0 rgba(0, 0, 0, 0.4)',
        padding: '10px 12px',
        zIndex: 10,
        display: 'flex',
        flexDirection: 'column',
        gap: '8px',
        minWidth: '140px',
        touchAction: 'none',
        userSelect: 'none',
        pointerEvents: 'auto'
      }}
    >
      {/* HEADER DRAG */}
      <div
        onMouseDown={startDrag}
        onTouchStart={startDrag}
        style={{
          display: 'flex',
          alignItems: 'center',
          cursor: 'move',
          padding: '6px 8px',
          backgroundColor: 'rgba(0, 0, 0, 0.35)',
          borderRadius: '8px',
          border: '1px solid rgba(255, 255, 255, 0.12)',
          width: '100%',
          boxSizing: 'border-box'
        }}
      >
        <svg width="12" height="12" viewBox="0 0 24 24" fill="rgba(255,255,255,0.6)" style={{ marginRight: '6px', flexShrink: 0 }}>
          <circle cx="9" cy="5" r="1.5" />
          <circle cx="9" cy="12" r="1.5" />
          <circle cx="9" cy="19" r="1.5" />
          <circle cx="15" cy="5" r="1.5" />
          <circle cx="15" cy="12" r="1.5" />
          <circle cx="15" cy="19" r="1.5" />
        </svg>

        <span
          style={{
            color: '#ffffff',
            fontSize: '11px',
            fontWeight: '600',
            letterSpacing: '0.3px',
            textShadow: '0 1px 3px rgba(0,0,0,0.9)',
            marginRight: 'auto'
          }}
        >
          {t('live.ptzControl')}
        </span>

        {onClose && (
          <button
            type="button"
            onClick={onClose}
            onMouseDown={(e) => e.stopPropagation()}
            onTouchStart={(e) => e.stopPropagation()}
            aria-label={t('common.close')}
            style={{
              background: 'none',
              border: 'none',
              cursor: 'pointer',
              padding: '2px',
              display: 'flex',
              alignItems: 'center',
              justifyContent: 'center',
              marginLeft: '6px',
              color: 'rgba(255, 255, 255, 0.7)',
              touchAction: 'manipulation'
            }}
          >
            <svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="2" strokeLinecap="round" strokeLinejoin="round">
              <line x1="18" y1="6" x2="6" y2="18" />
              <line x1="6" y1="6" x2="18" y2="18" />
            </svg>
          </button>
        )}
      </div>

      {/* Direction pad */}
      <div dir="ltr" style={{ display: 'grid', gridTemplateColumns: 'repeat(3, 40px)', gap: '4px', justifyContent: 'center' }}>
        <div></div>
        <DirectionButton
          direction="up"
          onMouseDown={() => handleMoveStart(0, 1, 0)}
          onMouseUp={handleMoveStop}
          onMouseLeave={handleMoveStop}
        />
        <div></div>

        <DirectionButton
          direction="left"
          onMouseDown={() => handleMoveStart(-1, 0, 0)}
          onMouseUp={handleMoveStop}
          onMouseLeave={handleMoveStop}
        />

        {/* HOME BUTTON */}
        <button
          type="button"
          onClick={handleHome}
          aria-label={t('live.goToHomePosition')}
          title={t('live.goToHomePosition')}
          style={{
            width: '32px',
            height: '32px',
            borderRadius: '50%',
            border: '1px solid rgba(59, 130, 246, 0.4)',
            backgroundColor: 'rgba(37, 99, 235, 0.25)',
            color: '#60a5fa',
            cursor: 'pointer',
            display: 'flex',
            alignItems: 'center',
            justifyContent: 'center',
            transition: 'all 0.15s ease',
            boxShadow: '0 2px 4px rgba(0, 0, 0, 0.2)'
          }}
        >
          <svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="2" strokeLinecap="round" strokeLinejoin="round">
            <path d="M3 9l9-7 9 7v11a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2z" />
            <polyline points="9 22 9 12 15 12 15 22" />
          </svg>
        </button>

        <DirectionButton
          direction="right"
          onMouseDown={() => handleMoveStart(1, 0, 0)}
          onMouseUp={handleMoveStop}
          onMouseLeave={handleMoveStop}
        />

        <div></div>
        <DirectionButton
          direction="down"
          onMouseDown={() => handleMoveStart(0, -1, 0)}
          onMouseUp={handleMoveStop}
          onMouseLeave={handleMoveStop}
        />
        <div></div>
      </div>

      {/* Zoom controls */}
      <div
        style={{
          display: 'flex',
          justifyContent: 'center',
          alignItems: 'center',
          gap: '8px',
          marginTop: '2px',
          width: '100%'
        }}
      >
        <DirectionButton
          direction="zoom-out"
          onMouseDown={() => handleMoveStart(0, 0, -1)}
          onMouseUp={handleMoveStop}
          onMouseLeave={handleMoveStop}
        />

        <span
          style={{
            color: '#ffffff',
            fontSize: '11px',
            fontWeight: '600',
            textShadow: '0 1px 3px rgba(0,0,0,0.9)'
          }}
        >
          {t('live.zoom')}
        </span>

        <DirectionButton
          direction="zoom-in"
          onMouseDown={() => handleMoveStart(0, 0, 1)}
          onMouseUp={handleMoveStop}
          onMouseLeave={handleMoveStop}
        />
      </div>

      {/* Speed slider */}
      <div
        style={{
          display: 'flex',
          alignItems: 'center',
          gap: '6px',
          marginTop: '2px',
          padding: '2px 4px'
        }}
      >
        <span style={{ color: '#ffffff', fontSize: '10px', fontWeight: '600', textShadow: '0 1px 3px rgba(0,0,0,0.9)' }}>
          {t('live.speedLabel')}
        </span>

        <input
          type="range"
          aria-label={t('live.speedLabel')}
          min="0.1"
          max="1"
          step="0.1"
          value={speed}
          onChange={(e) => setSpeed(parseFloat(e.target.value))}
          style={{ flex: 1, height: '4px', accentColor: '#3b82f6', cursor: 'pointer' }}
        />

        <span style={{ color: '#ffffff', fontSize: '10px', width: '26px', textAlign: 'right', fontWeight: '600', textShadow: '0 1px 3px rgba(0,0,0,0.9)' }}>
          {Math.round(speed * 100)}%
        </span>
      </div>

      {/* Presets */}
      {presets.length > 0 && (
        <div
          style={{
            display: 'flex',
            alignItems: 'stretch',
            gap: '4px',
            marginTop: '2px',
            width: '100%'
          }}
        >
          <select
            aria-label={t('live.goToPreset')}
            value={selectedPreset}
            onChange={(e) => {
              const token = e.target.value;
              setSelectedPreset(token);
              if (token) handlePreset(token);
            }}
            style={{
              flex: 1,
              minWidth: 0,
              padding: '5px 8px',
              borderRadius: '8px',
              border: '1px solid rgba(255, 255, 255, 0.25)',
              backgroundColor: 'rgba(0, 0, 0, 0.35)',
              color: 'white',
              fontSize: '10px',
              cursor: 'pointer',
              outline: 'none',
              boxSizing: 'border-box'
            }}
          >
            <option
              value=""
              style={{ backgroundColor: '#0f172a', color: 'white' }}
            >
              {t('live.goToPreset')}
            </option>
        
            {presets.map(preset => (
              <option
                key={preset.token}
                value={preset.token}
                style={{ backgroundColor: '#0f172a', color: 'white' }}
              >
                {preset.name || t('live.presetNumber', { token: preset.token })}
              </option>
            ))}
          </select>
        
          {canManage && selectedPreset && (
            <button
              type="button"
              onClick={handleDeletePreset}
              disabled={saving}
              title={t('common.delete') || 'Hapus Preset'}
              style={{
                width: '32px',
                minWidth: '32px',
                alignSelf: 'stretch',
                borderRadius: '8px',
                border: '1px solid rgba(239, 68, 68, 0.5)',
                backgroundColor: 'rgba(239, 68, 68, 0.4)',
                color: 'white',
                cursor: saving ? 'not-allowed' : 'pointer',
                display: 'flex',
                alignItems: 'center',
                justifyContent: 'center',
                flexShrink: 0,
                padding: '0',
                boxSizing: 'border-box',
                opacity: saving ? 0.5 : 1
              }}
            >
              <svg
                width="12"
                height="14"
                viewBox="0 0 24 24"
                fill="none"
                stroke="white"
                strokeWidth="2"
              >
                <polyline points="3 6 5 6 21 6"></polyline>
                <path d="M19 6v14a2 2 0 0 1-2 2H7a2 2 0 0 1-2-2V6m3 0V4a2 2 0 0 1 2-2h4a2 2 0 0 1 2 2v2"></path>
              </svg>
            </button>
          )}
        </div>
      )}
      
      {canManage && capabilities?.has_presets && (
        showPresetForm ? (
          <form onSubmit={handleSavePreset} style={{ display: 'flex', flexDirection: 'column', gap: '6px', width: '100%', marginTop: '2px' }}>
            <div style={{ display: 'flex', flexDirection: 'column', gap: '2px', width: '100%' }}>
              <label htmlFor="ptz-preset-name" style={{ color: '#ffffff', fontSize: '10px', fontWeight: '600', textShadow: '0 1px 3px rgba(0,0,0,0.9)' }}>
                {t('live.ptzPresetName')}
              </label>
              <input
                id="ptz-preset-name"
                type="text"
                value={presetName}
                onInput={(event) => setPresetName(event.currentTarget.value)}
                maxLength={64}
                required
                style={{
                  width: '100%',
                  padding: '5px 8px',
                  borderRadius: '8px',
                  border: '1px solid rgba(255, 255, 255, 0.25)',
                  backgroundColor: 'rgba(0, 0, 0, 0.4)',
                  color: 'white',
                  fontSize: '10px',
                  boxSizing: 'border-box',
                  outline: 'none'
                }}
              />
            </div>
            <div style={{ display: 'flex', gap: '4px' }}>
              <button 
                type="submit" 
                disabled={saving || !presetName.trim()} 
                aria-label={t('live.ptzSavePreset')} 
                style={{ 
                  flex: 1, 
                  fontSize: '10px', 
                  padding: '5px 8px', 
                  borderRadius: '8px', 
                  border: '1px solid rgba(59, 130, 246, 0.5)', 
                  backgroundColor: 'rgba(37, 99, 235, 0.7)', 
                  color: 'white', 
                  cursor: saving || !presetName.trim() ? 'not-allowed' : 'pointer',
                  opacity: saving || !presetName.trim() ? 0.5 : 1
                }}
              >
                {t('live.ptzSavePreset')}
              </button>
              <button 
                type="button" 
                disabled={saving} 
                onClick={() => setShowPresetForm(false)} 
                aria-label={t('common.cancel')} 
                style={{ 
                  flex: 1, 
                  fontSize: '10px', 
                  padding: '5px 8px', 
                  borderRadius: '8px', 
                  border: '1px solid rgba(255, 255, 255, 0.2)', 
                  backgroundColor: 'rgba(255, 255, 255, 0.15)', 
                  color: 'white', 
                  cursor: saving ? 'not-allowed' : 'pointer' 
                }}
              >
                {t('common.cancel')}
              </button>
            </div>
          </form>
        ) : (
          <button 
            type="button" 
            onClick={() => { setConfirmSetHome(false); setShowPresetForm(true); }} 
            aria-label={t('live.ptzSaveCurrentPosition')} 
            style={{ 
              width: '100%', 
              fontSize: '10px', 
              fontWeight: '500',
              padding: '6px 10px', 
              borderRadius: '8px', 
              border: '1px solid rgba(255, 255, 255, 0.15)', 
              backgroundColor: 'rgba(255, 255, 255, 0.08)', 
              color: 'white', 
              cursor: 'pointer',
              marginTop: '2px',
              display: 'flex',
              alignItems: 'center',
              justifyContent: 'center',
              gap: '6px',
              transition: 'all 0.15s ease'
            }}
          >
            <svg width="12" height="12" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="2" strokeLinecap="round" strokeLinejoin="round">
              <line x1="12" y1="5" x2="12" y2="19" />
              <line x1="5" y1="12" x2="19" y2="12" />
            </svg>
            <span>{t('live.ptzSaveCurrentPosition')}</span>
          </button>
        )
      )}

      {canManage && capabilities?.has_home_position && (
        confirmSetHome ? (
          <div style={{ color: '#ffffff', fontSize: '10px' }}>
            <span>{t('live.ptzConfirmSetHome')}</span>
            <div style={{ display: 'flex', gap: '4px', marginTop: '4px' }}>
              <button type="button" disabled={saving} onClick={handleSetHome} aria-label={t('live.ptzSetHome')} style={{ flex: 1, fontSize: '10px', padding: '4px 6px', borderRadius: '6px', border: '1px solid rgba(59, 130, 246, 0.5)', backgroundColor: 'rgba(37,99,235,0.7)', color: 'white' }}>
                {t('live.ptzSetHome')}
              </button>
              <button type="button" disabled={saving} onClick={() => setConfirmSetHome(false)} aria-label={t('common.cancel')} style={{ flex: 1, fontSize: '10px', padding: '4px 6px', borderRadius: '6px', border: '1px solid rgba(255,255,255,0.2)', backgroundColor: 'rgba(255,255,255,0.15)', color: 'white' }}>
                {t('common.cancel')}
              </button>
            </div>
          </div>
        ) : (
          <button 
            type="button" 
            onClick={() => { setShowPresetForm(false); setConfirmSetHome(true); }} 
            aria-label={t('live.ptzSetCurrentPositionHome')} 
            style={{ 
              width: '100%', 
              fontSize: '10px', 
              fontWeight: '500',
              padding: '6px 10px', 
              borderRadius: '8px', 
              border: '1px solid rgba(255, 255, 255, 0.15)', 
              backgroundColor: 'rgba(255, 255, 255, 0.08)', 
              color: 'white', 
              cursor: 'pointer',
              marginTop: '2px',
              display: 'flex',
              alignItems: 'center',
              justifyContent: 'center',
              gap: '6px',
              transition: 'all 0.15s ease'
            }}
          >
            <svg width="12" height="12" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="2" strokeLinecap="round" strokeLinejoin="round">
              <path d="M21 10c0 7-9 13-9 13s-9-6-9-13a9 9 0 0 1 18 0z" />
              <circle cx="12" cy="10" r="3" />
            </svg>
            <span>{t('live.ptzSetCurrentPositionHome')}</span>
          </button>
        )
      )}

      {/* Error message */}
      {error && (
        <div style={{ color: '#f87171', fontSize: '10px', textAlign: 'center', textShadow: '0 1px 3px rgba(0,0,0,0.9)' }}>
          {error}
        </div>
      )}
    </div>
  );
}
