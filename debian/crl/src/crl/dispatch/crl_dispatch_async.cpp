/*
This file is part of Telegram Desktop,
the official desktop version of Telegram messaging app, see https://telegram.org

Telegram Desktop is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

It is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
GNU General Public License for more details.

In addition, as a special exception, the copyright holders give permission
to link the code of portions of this program with the OpenSSL library.

Full license: https://github.com/telegramdesktop/tdesktop/blob/master/LICENSE
Copyright (c) 2014-2017 John Preston, https://desktop.telegram.org
*/
#include <crl/dispatch/crl_dispatch_async.h>

#ifdef CRL_USE_DISPATCH

#include <dispatch/dispatch.h>

namespace crl::details {

void *background_queue_dispatch() {
	return dispatch_get_global_queue(DISPATCH_QUEUE_PRIORITY_DEFAULT, 0);
}

void *main_queue_dispatch() {
	return dispatch_get_main_queue();
}

void on_queue_async(void *queue, void (*callable)(void*), void *argument) {
	dispatch_async_f(
		static_cast<dispatch_queue_t>(queue),
		argument,
		callable);
}

void on_queue_sync(void *queue, void (*callable)(void*), void *argument) {
	dispatch_sync_f(
		static_cast<dispatch_queue_t>(queue),
		argument,
		callable);
}

} // namespace crl::details

#endif // CRL_USE_DISPATCH
