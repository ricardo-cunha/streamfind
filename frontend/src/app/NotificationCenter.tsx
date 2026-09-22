import { useEffect, useRef, useState } from 'react';
import { subscribeAppNotificationHistory, subscribeAppNotifications, type AppNotification } from './notifications';

function iconFor(kind: AppNotification['kind']): string {
  return kind === 'success'
    ? 'fa-circle-check'
    : kind === 'error'
      ? 'fa-circle-exclamation'
      : kind === 'warning'
        ? 'fa-triangle-exclamation'
        : 'fa-circle-info';
}

function formatTime(value: number): string {
  return new Date(value).toLocaleTimeString([], { hour: '2-digit', minute: '2-digit' });
}

export function NotificationBell({ onOpen }: { onOpen: () => void }) {
  return (
    <button
      className="sf-notification-button"
      onClick={onOpen}
      aria-label="Open notification log"
      title="Notification log"
    >
      <i className="fa-solid fa-bell" />
    </button>
  );
}

export function NotificationToasts() {
  const [current, setCurrent] = useState<AppNotification | null>(null);
  const timerRef = useRef<number | null>(null);
  useEffect(() => {
    const unsubscribe = subscribeAppNotifications((notification) => {
      setCurrent(notification);
      if (timerRef.current !== null) window.clearTimeout(timerRef.current);
      timerRef.current = window.setTimeout(() => {
        setCurrent(null);
        timerRef.current = null;
      }, 5000);
    });
    return () => {
      unsubscribe();
      if (timerRef.current !== null) window.clearTimeout(timerRef.current);
    };
  }, []);
  if (!current) return null;
  return (
    <div className="sf-notification-toasts">
      <div className={`sf-notification-toast ${current.kind}`} role={current.kind === 'error' ? 'alert' : 'status'}>
        <i className={`fa-solid ${iconFor(current.kind)}`} />
        <span>{current.message}</span>
      </div>
    </div>
  );
}

export function NotificationsPane({ onClose }: { onClose: () => void }) {
  const [notifications, setNotifications] = useState<AppNotification[]>([]);
  useEffect(() => subscribeAppNotificationHistory(setNotifications), []);
  return (
    <div
      className="sf-side-pane-backdrop sf-notifications-backdrop"
      onMouseDown={(event) => {
        if (event.target === event.currentTarget) onClose();
      }}
    >
      <aside className="sf-side-pane sf-notifications-pane" aria-label="Notification log">
        <div className="sf-notifications-heading">
          <h2>Notification log</h2>
          <button className="sf-icon-button sf-close-button" onClick={onClose} aria-label="Close notifications">
            <i className="fa-solid fa-xmark" />
          </button>
        </div>
        <div className="sf-notifications-actions">
          <span>
            {notifications.length} {notifications.length === 1 ? 'entry' : 'entries'}
          </span>
        </div>
        {notifications.length === 0 ? (
          <div className="sf-notifications-empty">
            <i className="fa-regular fa-bell-slash" />
            <strong>No notifications</strong>
            <span>Application activity will be recorded here.</span>
          </div>
        ) : (
          <div className="sf-notification-list">
            {notifications.map((notification) => (
              <article key={notification.id} className="sf-notification-item">
                <i className={`fa-solid ${iconFor(notification.kind)}`} />
                <div>
                  <p>{notification.message}</p>
                  <time>{formatTime(notification.createdAt)}</time>
                </div>
              </article>
            ))}
          </div>
        )}
      </aside>
    </div>
  );
}
