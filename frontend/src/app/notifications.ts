export type NotificationKind = 'info' | 'success' | 'warning' | 'error';

export type AppNotification = {
  id: string;
  kind: NotificationKind;
  message: string;
  createdAt: number;
  read: boolean;
};

type Listener = (notification: AppNotification) => void;
type HistoryListener = (notifications: AppNotification[]) => void;

const history: AppNotification[] = [];
const listeners = new Set<Listener>();
const historyListeners = new Set<HistoryListener>();

function publishHistory(): void {
  const snapshot = history.map((notification) => ({ ...notification }));
  historyListeners.forEach((listener) => listener(snapshot));
}

export function notifyApp(input: { kind: NotificationKind; message: string }): AppNotification {
  const notification: AppNotification = {
    id: `${Date.now()}-${Math.random().toString(36).slice(2)}`,
    kind: input.kind,
    message: input.message,
    createdAt: Date.now(),
    read: false,
  };
  history.unshift(notification);
  if (history.length > 100) history.pop();
  listeners.forEach((listener) => listener(notification));
  publishHistory();
  return notification;
}

export function markAllNotificationsRead(): void {
  history.forEach((notification) => {
    notification.read = true;
  });
  publishHistory();
}

export function subscribeAppNotifications(listener: Listener): () => void {
  listeners.add(listener);
  return () => listeners.delete(listener);
}

export function subscribeAppNotificationHistory(listener: HistoryListener): () => void {
  historyListeners.add(listener);
  listener(history.map((notification) => ({ ...notification })));
  return () => historyListeners.delete(listener);
}
