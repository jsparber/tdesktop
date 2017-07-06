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
#include "profile/profile_channel_controllers.h"

#include "boxes/edit_participant_box.h"
#include "boxes/confirm_box.h"
#include "boxes/contacts_box.h"
#include "auth_session.h"
#include "apiwrap.h"
#include "lang/lang_keys.h"
#include "mainwidget.h"
#include "dialogs/dialogs_indexed_list.h"

namespace Profile {
namespace {

constexpr auto kParticipantsFirstPageCount = 16;
constexpr auto kParticipantsPerPage = 200;

} // namespace

ParticipantsBoxController::ParticipantsBoxController(gsl::not_null<ChannelData*> channel, Role role) : PeerListController((role == Role::Admins) ? nullptr : std::make_unique<BannedBoxSearchController>(channel, role, &_additional))
, _channel(channel)
, _role(role) {
	if (_channel->mgInfo) {
		_additional.creator = _channel->mgInfo->creator;
	}
}

void ParticipantsBoxController::Start(gsl::not_null<ChannelData*> channel, Role role) {
	auto controller = std::make_unique<ParticipantsBoxController>(channel, role);
	auto initBox = [role, channel, controller = controller.get()](PeerListBox *box) {
		box->addButton(langFactory(lng_close), [box] { box->closeBox(); });
		auto canAddNewItem = [role, channel] {
			switch (role) {
			case Role::Admins: return channel->canAddAdmins();
			case Role::Restricted:
			case Role::Kicked: return channel->canBanMembers();
			}
			Unexpected("Role value in ParticipantsBoxController::Start()");
		};
		auto addNewItemText = [role] {
			switch (role) {
			case Role::Admins: return langFactory(lng_channel_add_admin);
			case Role::Restricted: return langFactory(lng_channel_add_restricted);
			case Role::Kicked: return langFactory(lng_channel_add_banned);
			}
			Unexpected("Role value in ParticipantsBoxController::Start()");
		};
		if (canAddNewItem()) {
			box->addLeftButton(addNewItemText(), [controller] { controller->addNewItem(); });
		}
	};
	Ui::show(Box<PeerListBox>(std::move(controller), std::move(initBox)), KeepOtherLayers);
}

void ParticipantsBoxController::addNewItem() {
	auto weak = base::weak_unique_ptr<ParticipantsBoxController>(this);
	_addBox = Ui::show(Box<PeerListBox>(std::make_unique<AddParticipantBoxController>(_channel, _role, [weak](gsl::not_null<UserData*> user, const MTPChannelAdminRights &rights) {
		if (weak) {
			weak->editAdminDone(user, rights);
		}
	}, [weak](gsl::not_null<UserData*> user, const MTPChannelBannedRights &rights) {
		if (weak) {
			weak->editRestrictedDone(user, rights);
		}
	}), [](PeerListBox *box) {
		box->addButton(langFactory(lng_cancel), [box] { box->closeBox(); });
	}), KeepOtherLayers);
}

void ParticipantsBoxController::peerListSearchAddRow(gsl::not_null<PeerData*> peer) {
	Expects(_role != Role::Admins);
	PeerListController::peerListSearchAddRow(peer);
	if (_role == Role::Restricted && delegate()->peerListFullRowsCount() > 0) {
		setDescriptionText(QString());
	}
}

std::unique_ptr<PeerListRow> ParticipantsBoxController::createSearchRow(gsl::not_null<PeerData*> peer) {
	if (auto user = peer->asUser()) {
		return createRow(user);
	}
	return std::unique_ptr<PeerListRow>();
}

template <typename Callback>
void ParticipantsBoxController::HandleParticipant(const MTPChannelParticipant &participant, Role role, gsl::not_null<Additional*> additional, Callback callback) {
	if (role == Role::Admins && participant.type() == mtpc_channelParticipantAdmin) {
		auto &admin = participant.c_channelParticipantAdmin();
		if (auto user = App::userLoaded(admin.vuser_id.v)) {
			additional->adminRights[user] = admin.vadmin_rights;
			if (admin.is_can_edit()) {
				additional->adminCanEdit.emplace(user);
			} else {
				additional->adminCanEdit.erase(user);
			}
			if (auto promoted = App::userLoaded(admin.vpromoted_by.v)) {
				auto it = additional->adminPromotedBy.find(user);
				if (it == additional->adminPromotedBy.end()) {
					additional->adminPromotedBy.emplace(user, promoted);
				} else {
					it->second = promoted;
				}
			} else {
				LOG(("API Error: No user %1 for admin promoted by.").arg(admin.vpromoted_by.v));
			}
			callback(user);
		}
	} else if (role == Role::Admins && participant.type() == mtpc_channelParticipantCreator) {
		auto &creator = participant.c_channelParticipantCreator();
		if (auto user = App::userLoaded(creator.vuser_id.v)) {
			additional->creator = user;
			callback(user);
		}
	} else if ((role == Role::Restricted || role == Role::Kicked) && participant.type() == mtpc_channelParticipantBanned) {
		auto &banned = participant.c_channelParticipantBanned();
		if (auto user = App::userLoaded(banned.vuser_id.v)) {
			additional->restrictedRights[user] = banned.vbanned_rights;
			callback(user);
		}
	} else {
		LOG(("API Error: Bad participant type got while requesting for participants: %1").arg(participant.type()));
	}
}

void ParticipantsBoxController::prepare() {
	if (_role == Role::Admins) {
		delegate()->peerListSetSearchMode(PeerListSearchMode::Local);
		delegate()->peerListSetTitle(langFactory(lng_channel_admins));
	} else {
		delegate()->peerListSetSearchMode(PeerListSearchMode::Complex);
		delegate()->peerListSetTitle(langFactory((_role == Role::Restricted) ? lng_restricted_list_title : lng_banned_list_title));
	}
	setDescriptionText(lang(lng_contacts_loading));
	setSearchNoResultsText(lang(lng_blocked_list_not_found));
	delegate()->peerListRefreshRows();

	loadMoreRows();
}

void ParticipantsBoxController::loadMoreRows() {
	if (searchController() && searchController()->loadMoreRows()) {
		return;
	}
	if (_loadRequestId || _allLoaded) {
		return;
	}

	auto filter = [this] {
		if (_role == Role::Admins) {
			return MTP_channelParticipantsAdmins();
		} else if (_role == Role::Restricted) {
			return MTP_channelParticipantsBanned(MTP_string(QString()));
		}
		return MTP_channelParticipantsKicked(MTP_string(QString()));
	};

	// First query is small and fast, next loads a lot of rows.
	auto perPage = (_offset > 0) ? kParticipantsPerPage : kParticipantsFirstPageCount;
	_loadRequestId = request(MTPchannels_GetParticipants(_channel->inputChannel, filter(), MTP_int(_offset), MTP_int(perPage))).done([this](const MTPchannels_ChannelParticipants &result) {
		Expects(result.type() == mtpc_channels_channelParticipants);

		_loadRequestId = 0;

		if (!_offset) {
			setDescriptionText((_role == Role::Restricted) ? lang(lng_group_blocked_list_about) : QString());
		}
		auto &participants = result.c_channels_channelParticipants();
		App::feedUsers(participants.vusers);

		auto &list = participants.vparticipants.v;
		if (list.isEmpty()) {
			// To be sure - wait for a whole empty result list.
			_allLoaded = true;
		} else {
			for_const (auto &participant, list) {
				++_offset;
				HandleParticipant(participant, _role, &_additional, [this](gsl::not_null<UserData*> user) {
					appendRow(user);
				});
			}
		}
		delegate()->peerListRefreshRows();
	}).fail([this](const RPCError &error) {
		_loadRequestId = 0;
	}).send();
}

void ParticipantsBoxController::rowClicked(gsl::not_null<PeerListRow*> row) {
	Ui::showPeerHistoryAsync(row->peer()->id, ShowAtUnreadMsgId);
}

void ParticipantsBoxController::rowActionClicked(gsl::not_null<PeerListRow*> row) {
	auto user = row->peer()->asUser();
	Expects(user != nullptr);

	if (_role == Role::Admins) {
		editAdmin(user);
	} else if (_role == Role::Restricted) {
		editRestricted(user);
	} else {
		removeKicked(row, user);
	}
}

void ParticipantsBoxController::editAdmin(gsl::not_null<UserData*> user) {
	if (_additional.adminCanEdit.find(user) == _additional.adminCanEdit.end()) {
		return;
	}

	auto it = _additional.adminRights.find(user);
	t_assert(it != _additional.adminRights.cend());
	auto weak = base::weak_unique_ptr<ParticipantsBoxController>(this);
	auto hasAdminRights = true;
	_editBox = Ui::show(Box<EditAdminBox>(_channel, user, hasAdminRights, it->second, [megagroup = _channel.get(), user, weak](const MTPChannelAdminRights &rights) {
		MTP::send(MTPchannels_EditAdmin(megagroup->inputChannel, user->inputUser, rights), rpcDone([megagroup, user, weak, rights](const MTPUpdates &result) {
			AuthSession::Current().api().applyUpdates(result);
			megagroup->applyEditAdmin(user, rights);
			if (weak) {
				weak->editAdminDone(user, rights);
			}
		}));
	}), KeepOtherLayers);
}

void ParticipantsBoxController::editAdminDone(gsl::not_null<UserData*> user, const MTPChannelAdminRights &rights) {
	if (_editBox) {
		_editBox->closeBox();
	}
	if (_addBox) {
		_addBox->closeBox();
	}
	auto notAdmin = (rights.c_channelAdminRights().vflags.v == 0);
	if (notAdmin) {
		_additional.adminRights.erase(user);
		_additional.adminPromotedBy.erase(user);
		_additional.adminCanEdit.erase(user);
		if (_role == Role::Admins) {
			removeRow(user);
		}
	} else {
		// It won't be replaced if the entry already exists.
		_additional.adminPromotedBy.emplace(user, App::self());
		_additional.adminCanEdit.emplace(user);
		_additional.adminRights[user] = rights;
		_additional.kicked.erase(user);
		_additional.restrictedRights.erase(user);
		if (_role == Role::Admins) {
			prependRow(user);
		} else {
			removeRow(user);
		}
	}
	delegate()->peerListRefreshRows();
}

void ParticipantsBoxController::editRestricted(gsl::not_null<UserData*> user) {
	auto it = _additional.restrictedRights.find(user);
	t_assert(it != _additional.restrictedRights.cend());
	auto weak = base::weak_unique_ptr<ParticipantsBoxController>(this);
	auto hasAdminRights = false;
	_editBox = Ui::show(Box<EditRestrictedBox>(_channel, user, hasAdminRights, it->second, [megagroup = _channel.get(), user, weak](const MTPChannelBannedRights &rights) {
		MTP::send(MTPchannels_EditBanned(megagroup->inputChannel, user->inputUser, rights), rpcDone([megagroup, user, weak, rights](const MTPUpdates &result) {
			AuthSession::Current().api().applyUpdates(result);
			megagroup->applyEditBanned(user, rights);
			if (weak) {
				weak->editRestrictedDone(user, rights);
			}
		}));
	}), KeepOtherLayers);
}

void ParticipantsBoxController::editRestrictedDone(gsl::not_null<UserData*> user, const MTPChannelBannedRights &rights) {
	if (_editBox) {
		_editBox->closeBox();
	}
	if (_addBox) {
		_addBox->closeBox();
	}
	auto notBanned = (rights.c_channelBannedRights().vflags.v == 0);
	auto fullBanned = rights.c_channelBannedRights().is_view_messages();
	if (notBanned) {
		_additional.kicked.erase(user);
		_additional.restrictedRights.erase(user);
		if (_role != Role::Admins) {
			removeRow(user);
		}
	} else {
		_additional.adminRights.erase(user);
		_additional.adminCanEdit.erase(user);
		_additional.adminPromotedBy.erase(user);
		if (fullBanned) {
			_additional.kicked.emplace(user);
			_additional.restrictedRights.erase(user);
			if (_role == Role::Kicked) {
				prependRow(user);
			} else {
				removeRow(user);
			}
		} else {
			_additional.restrictedRights[user] = rights;
			_additional.kicked.erase(user);
			if (_role == Role::Restricted) {
				prependRow(user);
			} else {
				removeRow(user);
			}
		}
	}
	delegate()->peerListRefreshRows();
}

void ParticipantsBoxController::removeKicked(gsl::not_null<PeerListRow*> row, gsl::not_null<UserData*> user) {
	delegate()->peerListRemoveRow(row);
	delegate()->peerListRefreshRows();

	AuthSession::Current().api().unblockParticipant(_channel, user);
}

bool ParticipantsBoxController::appendRow(gsl::not_null<UserData*> user) {
	if (delegate()->peerListFindRow(user->id)) {
		return false;
	}
	delegate()->peerListAppendRow(createRow(user));
	if (_role != Role::Kicked) {
		setDescriptionText(QString());
	}
	return true;
}

bool ParticipantsBoxController::prependRow(gsl::not_null<UserData*> user) {
	if (delegate()->peerListFindRow(user->id)) {
		return false;
	}
	delegate()->peerListPrependRow(createRow(user));
	if (_role != Role::Kicked) {
		setDescriptionText(QString());
	}
	return true;
}

bool ParticipantsBoxController::removeRow(gsl::not_null<UserData*> user) {
	if (auto row = delegate()->peerListFindRow(user->id)) {
		delegate()->peerListRemoveRow(row);
		if (!delegate()->peerListFullRowsCount()) {
			setDescriptionText(lang(lng_blocked_list_not_found));
		}
		return true;
	}
	return false;
}

std::unique_ptr<PeerListRow> ParticipantsBoxController::createRow(gsl::not_null<UserData*> user) const {
	auto row = std::make_unique<PeerListRowWithLink>(user);
	if (_role == Role::Admins) {
		auto promotedBy = _additional.adminPromotedBy.find(user);
		if (promotedBy == _additional.adminPromotedBy.end()) {
			row->setCustomStatus(lang(lng_channel_admin_status_creator));
		} else {
			row->setCustomStatus(lng_channel_admin_status_promoted_by(lt_user, App::peerName(promotedBy->second)));
		}
	}
	if (_role == Role::Restricted || (_role == Role::Admins && _additional.adminCanEdit.find(user) != _additional.adminCanEdit.end())) {
		row->setActionLink(lang(lng_profile_edit_permissions));
	} else if (_role == Role::Kicked) {
		row->setActionLink(lang(lng_blocked_list_unblock));
	}
	return std::move(row);
}

BannedBoxSearchController::BannedBoxSearchController(gsl::not_null<ChannelData*> channel, Role role, gsl::not_null<Additional*> additional)
: _channel(channel)
, _role(role)
, _additional(additional) {
	Expects(role != Role::Admins);
	_timer.setCallback([this] { searchOnServer(); });
}

void BannedBoxSearchController::searchQuery(const QString &query) {
	if (_query != query) {
		_query = query;
		_offset = 0;
		_requestId = 0;
		_allLoaded = false;
		if (!_query.isEmpty() && !searchInCache()) {
			_timer.callOnce(AutoSearchTimeout);
		} else {
			_timer.cancel();
		}
	}
}

void BannedBoxSearchController::searchOnServer() {
	Expects(!_query.isEmpty());
	loadMoreRows();
}

bool BannedBoxSearchController::isLoading() {
	return _timer.isActive() || _requestId;
}

bool BannedBoxSearchController::searchInCache() {
	auto it = _cache.find(_query);
	if (it != _cache.cend()) {
		_requestId = 0;
		searchDone(_requestId, it->second.result, it->second.requestedCount);
		return true;
	}
	return false;
}

bool BannedBoxSearchController::loadMoreRows() {
	if (_query.isEmpty()) {
		return false;
	}
	if (!_allLoaded && !isLoading()) {
		auto filter = (_role == Role::Restricted) ? MTP_channelParticipantsBanned(MTP_string(_query)) : MTP_channelParticipantsKicked(MTP_string(_query));

		// For search we request a lot of rows from the first query.
		// (because we've waited for search request by timer already,
		// so we don't expect it to be fast, but we want to fill cache).
		auto perPage = kParticipantsPerPage;
		_requestId = request(MTPchannels_GetParticipants(_channel->inputChannel, filter, MTP_int(_offset), MTP_int(perPage))).done([this, perPage](const MTPchannels_ChannelParticipants &result, mtpRequestId requestId) {
			searchDone(requestId, result, perPage);
		}).fail([this](const RPCError &error, mtpRequestId requestId) {
			if (_requestId == requestId) {
				_requestId = 0;
				_allLoaded = true;
				delegate()->peerListSearchRefreshRows();
			}
		}).send();

		auto entry = Query();
		entry.text = _query;
		entry.offset = _offset;
		_queries.emplace(_requestId, entry);
	}
	return true;
}

void BannedBoxSearchController::searchDone(mtpRequestId requestId, const MTPchannels_ChannelParticipants &result, int requestedCount) {
	Expects(result.type() == mtpc_channels_channelParticipants);

	auto &participants = result.c_channels_channelParticipants();
	auto query = _query;
	if (requestId) {
		App::feedUsers(participants.vusers);
		auto it = _queries.find(requestId);
		if (it != _queries.cend()) {
			query = it->second.text;
			if (it->second.offset == 0) {
				auto &entry = _cache[query];
				entry.result = result;
				entry.requestedCount = requestedCount;
			}
			_queries.erase(it);
		}
	}

	if (_requestId == requestId) {
		_requestId = 0;
		auto &list = participants.vparticipants.v;
		if (list.size() < requestedCount) {
			// We want cache to have full information about a query with small
			// results count (if we don't need the second request). So we don't
			// wait for an empty results list unlike the non-search peer list.
			_allLoaded = true;
		}
		for_const (auto &participant, list) {
			ParticipantsBoxController::HandleParticipant(participant, _role, _additional, [this](gsl::not_null<UserData*> user) {
				delegate()->peerListSearchAddRow(user);
			});
		}
		_offset += list.size();
		delegate()->peerListSearchRefreshRows();
	}
}

AddParticipantBoxController::AddParticipantBoxController(gsl::not_null<ChannelData*> channel, Role role, AdminDoneCallback adminDoneCallback, BannedDoneCallback bannedDoneCallback) : PeerListController(std::make_unique<AddParticipantBoxSearchController>(channel, &_additional))
, _channel(channel)
, _role(role)
, _adminDoneCallback(std::move(adminDoneCallback))
, _bannedDoneCallback(std::move(bannedDoneCallback)) {
	if (_channel->mgInfo) {
		_additional.creator = _channel->mgInfo->creator;
	}
}

std::unique_ptr<PeerListRow> AddParticipantBoxController::createSearchRow(gsl::not_null<PeerData*> peer) {
	if (!peer->isSelf()) {
		if (auto user = peer->asUser()) {
			return createRow(user);
		}
	}
	return std::unique_ptr<PeerListRow>();
}

void AddParticipantBoxController::prepare() {
	delegate()->peerListSetSearchMode(PeerListSearchMode::Complex);
	auto title = [this] {
		switch (_role) {
		case Role::Admins: return langFactory(lng_channel_add_admin);
		case Role::Restricted: return langFactory(lng_channel_add_restricted);
		case Role::Kicked: return langFactory(lng_channel_add_banned);
		}
		Unexpected("Role in AddParticipantBoxController::prepare()");
	};
	delegate()->peerListSetTitle(title());
	setDescriptionText(lang(lng_contacts_loading));
	setSearchNoResultsText(lang(lng_blocked_list_not_found));
	delegate()->peerListRefreshRows();

	loadMoreRows();
}

void AddParticipantBoxController::loadMoreRows() {
	if (searchController() && searchController()->loadMoreRows()) {
		return;
	}
	if (_loadRequestId || _allLoaded) {
		return;
	}

	// First query is small and fast, next loads a lot of rows.
	auto perPage = (_offset > 0) ? kParticipantsPerPage : kParticipantsFirstPageCount;
	_loadRequestId = request(MTPchannels_GetParticipants(_channel->inputChannel, MTP_channelParticipantsRecent(), MTP_int(_offset), MTP_int(perPage))).done([this](const MTPchannels_ChannelParticipants &result) {
		Expects(result.type() == mtpc_channels_channelParticipants);

		_loadRequestId = 0;

		auto &participants = result.c_channels_channelParticipants();
		App::feedUsers(participants.vusers);

		auto &list = participants.vparticipants.v;
		if (list.isEmpty()) {
			// To be sure - wait for a whole empty result list.
			_allLoaded = true;
		} else {
			for_const (auto &participant, list) {
				++_offset;
				HandleParticipant(participant, &_additional, [this](gsl::not_null<UserData*> user) {
					appendRow(user);
				});
			}
		}
		if (delegate()->peerListFullRowsCount() > 0) {
			setDescriptionText(QString());
		} else if (_allLoaded) {
			setDescriptionText(lang(lng_blocked_list_not_found));
		}
		delegate()->peerListRefreshRows();
	}).fail([this](const RPCError &error) {
		_loadRequestId = 0;
	}).send();
}

void AddParticipantBoxController::rowClicked(gsl::not_null<PeerListRow*> row) {
	auto user = row->peer()->asUser();
	switch (_role) {
	case Role::Admins: return editAdmin(user);
	case Role::Restricted: return editRestricted(user);
	case Role::Kicked: return kickUser(user);
	}
	Unexpected("Role in AddParticipantBoxController::rowClicked()");
}

template <typename Callback>
bool AddParticipantBoxController::checkInfoLoaded(gsl::not_null<UserData*> user, Callback callback) {
	if (_additional.infoNotLoaded.find(user) == _additional.infoNotLoaded.end()) {
		return true;
	}

	// We don't know what this user status is in the group.
	request(MTPchannels_GetParticipant(_channel->inputChannel, user->inputUser)).done([this, user, callback](const MTPchannels_ChannelParticipant &result) {
		Expects(result.type() == mtpc_channels_channelParticipant);
		auto &participant = result.c_channels_channelParticipant();
		App::feedUsers(participant.vusers);
		HandleParticipant(participant.vparticipant, &_additional, [](gsl::not_null<UserData*>) {});
		_additional.infoNotLoaded.erase(user);
		callback();
	}).fail([this, user, callback](const RPCError &error) {
		_additional.infoNotLoaded.erase(user);
		_additional.external.emplace(user);
		callback();
	}).send();
	return false;
}

void AddParticipantBoxController::editAdmin(gsl::not_null<UserData*> user, bool sure) {
	if (!checkInfoLoaded(user, [this, user] { editAdmin(user); })) {
		return;
	}

	if (sure && _editBox) {
		// Close the confirmation box.
		_editBox->closeBox();
	}

	// Check restrictions.
	auto weak = base::weak_unique_ptr<AddParticipantBoxController>(this);
	auto alreadyIt = _additional.adminRights.find(user);
	auto hasAdminRights = false;
	auto currentRights = EditAdminBox::DefaultRights(_channel);
	if (alreadyIt != _additional.adminRights.end() || _additional.creator == user) {
		// The user is already an admin.
		if (_additional.adminCanEdit.find(user) == _additional.adminCanEdit.end() || _additional.creator == user) {
			Ui::show(Box<InformBox>(lang(lng_error_cant_edit_admin)), KeepOtherLayers);
			return;
		}
		hasAdminRights = true;
		currentRights = alreadyIt->second;
	} else if (_additional.kicked.find(user) != _additional.kicked.end()) {
		// The user is banned.
		if (_channel->canAddMembers()) {
			if (_channel->canBanMembers()) {
				if (!sure) {
					_editBox = Ui::show(Box<ConfirmBox>(lang(lng_sure_add_admin_unban), [weak, user] { weak->editAdmin(user, true); }), KeepOtherLayers);
					return;
				}
			} else {
				Ui::show(Box<InformBox>(lang(lng_error_cant_add_admin_unban)), KeepOtherLayers);
				return;
			}
		} else {
			Ui::show(Box<InformBox>(lang(lng_error_cant_add_admin_invite)), KeepOtherLayers);
			return;
		}
	} else if (_additional.restrictedRights.find(user) != _additional.restrictedRights.end()) {
		// The user is restricted.
		if (_channel->canBanMembers()) {
			if (!sure) {
				_editBox = Ui::show(Box<ConfirmBox>(lang(lng_sure_add_admin_unban), [weak, user] { weak->editAdmin(user, true); }), KeepOtherLayers);
				return;
			}
		} else {
			Ui::show(Box<InformBox>(lang(lng_error_cant_add_admin_unban)), KeepOtherLayers);
			return;
		}
	} else if (_additional.external.find(user) != _additional.external.end()) {
		// The user is not in the group yet.
		if (_channel->canAddMembers()) {
			if (!sure) {
				_editBox = Ui::show(Box<ConfirmBox>(lang(lng_sure_add_admin_invite), [weak, user] { weak->editAdmin(user, true); }), KeepOtherLayers);
				return;
			}
		} else {
			Ui::show(Box<InformBox>(lang(lng_error_cant_add_admin_invite)), KeepOtherLayers);
			return;
		}
	}

	// Finally edit the admin.
	_editBox = Ui::show(Box<EditAdminBox>(_channel, user, hasAdminRights, currentRights, [channel = _channel.get(), user, weak](const MTPChannelAdminRights &rights) {
		MTP::send(MTPchannels_EditAdmin(channel->inputChannel, user->inputUser, rights), rpcDone([channel, user, weak, rights](const MTPUpdates &result) {
			AuthSession::Current().api().applyUpdates(result);
			channel->applyEditAdmin(user, rights);
			if (weak) {
				weak->editAdminDone(user, rights);
			}
		}), rpcFail([channel](const RPCError &error) {
			if (MTP::isDefaultHandledError(error)) {
				return false;
			}
			if (error.type() == qstr("USER_NOT_MUTUAL_CONTACT")) {
				Ui::show(Box<InformBox>(PeerFloodErrorText(channel->isMegagroup() ? PeerFloodType::InviteGroup : PeerFloodType::InviteChannel)), KeepOtherLayers);
			}
			return true;
		}));
	}), KeepOtherLayers);
}

void AddParticipantBoxController::editAdminDone(gsl::not_null<UserData*> user, const MTPChannelAdminRights &rights) {
	if (_editBox) _editBox->closeBox();
	_additional.restrictedRights.erase(user);
	_additional.kicked.erase(user);
	_additional.external.erase(user);
	if (rights.c_channelAdminRights().vflags.v == 0) {
		_additional.adminRights.erase(user);
		_additional.adminPromotedBy.erase(user);
		_additional.adminCanEdit.erase(user);
	} else {
		_additional.adminRights[user] = rights;
		_additional.adminCanEdit.emplace(user);
		auto it = _additional.adminPromotedBy.find(user);
		if (it == _additional.adminPromotedBy.end()) {
			_additional.adminPromotedBy.emplace(user, App::self());
		}
	}
	if (_adminDoneCallback) {
		_adminDoneCallback(user, rights);
	}
}

void AddParticipantBoxController::editRestricted(gsl::not_null<UserData*> user, bool sure) {
	if (!checkInfoLoaded(user, [this, user] { editRestricted(user); })) {
		return;
	}

	if (sure && _editBox) {
		// Close the confirmation box.
		_editBox->closeBox();
	}

	// Check restrictions.
	auto weak = base::weak_unique_ptr<AddParticipantBoxController>(this);
	auto alreadyIt = _additional.restrictedRights.find(user);
	auto currentRights = EditRestrictedBox::DefaultRights(_channel);
	auto hasAdminRights = false;
	if (alreadyIt != _additional.restrictedRights.end()) {
		// The user is already banned or restricted.
		currentRights = alreadyIt->second;
	} else if (_additional.adminRights.find(user) != _additional.adminRights.end() || _additional.creator == user) {
		// The user is an admin or creator.
		if (_additional.adminCanEdit.find(user) != _additional.adminCanEdit.end()) {
			hasAdminRights = true;
			if (!sure) {
				_editBox = Ui::show(Box<ConfirmBox>(lang(lng_sure_ban_admin), [weak, user] { weak->editRestricted(user, true); }), KeepOtherLayers);
				return;
			}
		} else {
			Ui::show(Box<InformBox>(lang(lng_error_cant_ban_admin)), KeepOtherLayers);
			return;
		}
	}

	// Finally edit the restricted.
	_editBox = Ui::show(Box<EditRestrictedBox>(_channel, user, hasAdminRights, currentRights, [user, weak](const MTPChannelBannedRights &rights) {
		if (weak) {
			weak->restrictUserSure(user, rights);
		}
	}), KeepOtherLayers);
}

void AddParticipantBoxController::restrictUserSure(gsl::not_null<UserData*> user, const MTPChannelBannedRights &rights) {
	auto weak = base::weak_unique_ptr<AddParticipantBoxController>(this);
	MTP::send(MTPchannels_EditBanned(_channel->inputChannel, user->inputUser, rights), rpcDone([megagroup = _channel.get(), user, weak, rights](const MTPUpdates &result) {
		AuthSession::Current().api().applyUpdates(result);
		megagroup->applyEditBanned(user, rights);
		if (weak) {
			weak->editRestrictedDone(user, rights);
		}
	}));
}

void AddParticipantBoxController::editRestrictedDone(gsl::not_null<UserData*> user, const MTPChannelBannedRights &rights) {
	if (_editBox) _editBox->closeBox();
	_additional.adminRights.erase(user);
	_additional.adminCanEdit.erase(user);
	_additional.adminPromotedBy.erase(user);
	if (rights.c_channelBannedRights().vflags.v == 0) {
		_additional.restrictedRights.erase(user);
		_additional.kicked.erase(user);
	} else {
		_additional.restrictedRights[user] = rights;
		if (rights.c_channelBannedRights().vflags.v & MTPDchannelBannedRights::Flag::f_view_messages) {
			_additional.kicked.emplace(user);
		} else {
			_additional.kicked.erase(user);
		}
	}
	if (_bannedDoneCallback) {
		_bannedDoneCallback(user, rights);
	}
}

void AddParticipantBoxController::kickUser(gsl::not_null<UserData*> user, bool sure) {
	if (!checkInfoLoaded(user, [this, user] { kickUser(user); })) {
		return;
	}

	// Check restrictions.
	auto weak = base::weak_unique_ptr<AddParticipantBoxController>(this);
	if (_additional.adminRights.find(user) != _additional.adminRights.end() || _additional.creator == user) {
		// The user is an admin or creator.
		if (_additional.adminCanEdit.find(user) != _additional.adminCanEdit.end()) {
			if (!sure) {
				_editBox = Ui::show(Box<ConfirmBox>(lang(lng_sure_ban_admin), [weak, user] { weak->kickUser(user, true); }), KeepOtherLayers);
				return;
			}
		} else {
			Ui::show(Box<InformBox>(lang(lng_error_cant_ban_admin)), KeepOtherLayers);
			return;
		}
	}

	// Finally kick him.
	if (!sure) {
		auto text = lng_sure_ban_user_group(lt_user, App::peerName(user));
		_editBox = Ui::show(Box<ConfirmBox>(text, [weak, user] { weak->kickUser(user, true); }), KeepOtherLayers);
		return;
	}
	restrictUserSure(user, ChannelData::KickedRestrictedRights());
}

bool AddParticipantBoxController::appendRow(gsl::not_null<UserData*> user) {
	if (delegate()->peerListFindRow(user->id) || user->isSelf()) {
		return false;
	}
	delegate()->peerListAppendRow(createRow(user));
	return true;
}

bool AddParticipantBoxController::prependRow(gsl::not_null<UserData*> user) {
	if (delegate()->peerListFindRow(user->id)) {
		return false;
	}
	delegate()->peerListPrependRow(createRow(user));
	return true;
}

std::unique_ptr<PeerListRow> AddParticipantBoxController::createRow(gsl::not_null<UserData*> user) const {
	return std::make_unique<PeerListRow>(user);
}

template <typename Callback>
void AddParticipantBoxController::HandleParticipant(const MTPChannelParticipant &participant, gsl::not_null<Additional*> additional, Callback callback) {
	switch (participant.type()) {
	case mtpc_channelParticipantAdmin: {
		auto &admin = participant.c_channelParticipantAdmin();
		if (auto user = App::userLoaded(admin.vuser_id.v)) {
			additional->infoNotLoaded.erase(user);
			additional->restrictedRights.erase(user);
			additional->kicked.erase(user);
			additional->adminRights[user] = admin.vadmin_rights;
			if (admin.is_can_edit()) {
				additional->adminCanEdit.emplace(user);
			} else {
				additional->adminCanEdit.erase(user);
			}
			if (auto promoted = App::userLoaded(admin.vpromoted_by.v)) {
				auto it = additional->adminPromotedBy.find(user);
				if (it == additional->adminPromotedBy.end()) {
					additional->adminPromotedBy.emplace(user, promoted);
				} else {
					it->second = promoted;
				}
			} else {
				LOG(("API Error: No user %1 for admin promoted by.").arg(admin.vpromoted_by.v));
			}
			callback(user);
		}
	} break;
	case mtpc_channelParticipantCreator: {
		auto &creator = participant.c_channelParticipantCreator();
		if (auto user = App::userLoaded(creator.vuser_id.v)) {
			additional->infoNotLoaded.erase(user);
			additional->creator = user;
			callback(user);
		}
	} break;
	case mtpc_channelParticipantBanned: {
		auto &banned = participant.c_channelParticipantBanned();
		if (auto user = App::userLoaded(banned.vuser_id.v)) {
			additional->infoNotLoaded.erase(user);
			additional->adminRights.erase(user);
			additional->adminCanEdit.erase(user);
			additional->adminPromotedBy.erase(user);
			if (banned.is_left()) {
				additional->kicked.emplace(user);
			} else {
				additional->kicked.erase(user);
			}
			additional->restrictedRights[user] = banned.vbanned_rights;
			callback(user);
		}
	} break;
	case mtpc_channelParticipant: {
		auto &data = participant.c_channelParticipant();
		if (auto user = App::userLoaded(data.vuser_id.v)) {
			additional->infoNotLoaded.erase(user);
			additional->adminRights.erase(user);
			additional->adminCanEdit.erase(user);
			additional->adminPromotedBy.erase(user);
			additional->restrictedRights.erase(user);
			additional->kicked.erase(user);
			callback(user);
		}
	} break;
	default: Unexpected("Participant type in AddParticipantBoxController::HandleParticipant()");
	}
}

AddParticipantBoxSearchController::AddParticipantBoxSearchController(gsl::not_null<ChannelData*> channel, gsl::not_null<Additional*> additional)
: _channel(channel)
, _additional(additional) {
	_timer.setCallback([this] { searchOnServer(); });
}

void AddParticipantBoxSearchController::searchQuery(const QString &query) {
	if (_query != query) {
		_query = query;
		_offset = 0;
		_requestId = 0;
		_participantsLoaded = false;
		_chatsContactsAdded = false;
		_globalLoaded = false;
		if (!_query.isEmpty() && !searchParticipantsInCache()) {
			_timer.callOnce(AutoSearchTimeout);
		} else {
			_timer.cancel();
		}
	}
}

void AddParticipantBoxSearchController::searchOnServer() {
	Expects(!_query.isEmpty());
	loadMoreRows();
}

bool AddParticipantBoxSearchController::isLoading() {
	return _timer.isActive() || _requestId;
}

bool AddParticipantBoxSearchController::searchParticipantsInCache() {
	auto it = _participantsCache.find(_query);
	if (it != _participantsCache.cend()) {
		_requestId = 0;
		searchParticipantsDone(_requestId, it->second.result, it->second.requestedCount);
		return true;
	}
	return false;
}

bool AddParticipantBoxSearchController::searchGlobalInCache() {
	auto it = _globalCache.find(_query);
	if (it != _globalCache.cend()) {
		_requestId = 0;
		searchGlobalDone(_requestId, it->second);
		return true;
	}
	return false;
}

bool AddParticipantBoxSearchController::loadMoreRows() {
	if (_query.isEmpty()) {
		return false;
	}
	if (_globalLoaded) {
		return true;
	}
	if (_participantsLoaded) {
		if (!_chatsContactsAdded) {
			addChatsContacts();
		}
		if (!isLoading() && !searchGlobalInCache()) {
			requestGlobal();
		}
	} else if (!isLoading()) {
		requestParticipants();
	}
	return true;
}

void AddParticipantBoxSearchController::requestParticipants() {
	// For search we request a lot of rows from the first query.
	// (because we've waited for search request by timer already,
	// so we don't expect it to be fast, but we want to fill cache).
	auto perPage = kParticipantsPerPage;
	_requestId = request(MTPchannels_GetParticipants(_channel->inputChannel, MTP_channelParticipantsSearch(MTP_string(_query)), MTP_int(_offset), MTP_int(perPage))).done([this, perPage](const MTPchannels_ChannelParticipants &result, mtpRequestId requestId) {
		searchParticipantsDone(requestId, result, perPage);
	}).fail([this](const RPCError &error, mtpRequestId requestId) {
		if (_requestId == requestId) {
			_requestId = 0;
			_participantsLoaded = true;
			loadMoreRows();
			delegate()->peerListSearchRefreshRows();
		}
	}).send();
	auto entry = Query();
	entry.text = _query;
	entry.offset = _offset;
	_participantsQueries.emplace(_requestId, entry);
}

void AddParticipantBoxSearchController::searchParticipantsDone(mtpRequestId requestId, const MTPchannels_ChannelParticipants &result, int requestedCount) {
	Expects(result.type() == mtpc_channels_channelParticipants);

	auto &participants = result.c_channels_channelParticipants();
	auto query = _query;
	if (requestId) {
		App::feedUsers(participants.vusers);
		auto it = _participantsQueries.find(requestId);
		if (it != _participantsQueries.cend()) {
			query = it->second.text;
			if (it->second.offset == 0) {
				auto &entry = _participantsCache[query];
				entry.result = result;
				entry.requestedCount = requestedCount;
			}
			_participantsQueries.erase(it);
		}
	}

	if (_requestId == requestId) {
		_requestId = 0;
		auto &list = participants.vparticipants.v;
		if (list.size() < requestedCount) {
			// We want cache to have full information about a query with small
			// results count (if we don't need the second request). So we don't
			// wait for an empty results list unlike the non-search peer list.
			_participantsLoaded = true;
			if (list.empty() && _offset == 0) {
				// No results, so we want to request global search immediately.
				loadMoreRows();
			}
		}
		for_const (auto &participant, list) {
			AddParticipantBoxController::HandleParticipant(participant, _additional, [this](gsl::not_null<UserData*> user) {
				delegate()->peerListSearchAddRow(user);
			});
		}
		_offset += list.size();
		delegate()->peerListSearchRefreshRows();
	}
}

void AddParticipantBoxSearchController::requestGlobal() {
	if (_query.size() < MinUsernameLength) {
		_globalLoaded = true;
		return;
	}

	auto perPage = SearchPeopleLimit;
	_requestId = request(MTPcontacts_Search(MTP_string(_query), MTP_int(perPage))).done([this](const MTPcontacts_Found &result, mtpRequestId requestId) {
		searchGlobalDone(requestId, result);
	}).fail([this](const RPCError &error, mtpRequestId requestId) {
		if (_requestId == requestId) {
			_requestId = 0;
			_globalLoaded = true;
			delegate()->peerListSearchRefreshRows();
		}
	}).send();
	_globalQueries.emplace(_requestId, _query);
}

void AddParticipantBoxSearchController::searchGlobalDone(mtpRequestId requestId, const MTPcontacts_Found &result) {
	Expects(result.type() == mtpc_contacts_found);

	auto &found = result.c_contacts_found();
	auto query = _query;
	if (requestId) {
		App::feedUsers(found.vusers);
		App::feedChats(found.vchats);
		auto it = _globalQueries.find(requestId);
		if (it != _globalQueries.cend()) {
			query = it->second;
			_globalCache[query] = result;
			_globalQueries.erase(it);
		}
	}

	if (_requestId == requestId) {
		_requestId = 0;
		_globalLoaded = true;
		for_const (auto &mtpPeer, found.vresults.v) {
			auto peerId = peerFromMTP(mtpPeer);
			if (auto peer = App::peerLoaded(peerId)) {
				if (auto user = peer->asUser()) {
					if (_additional->adminRights.find(user) == _additional->adminRights.cend()
						&& _additional->restrictedRights.find(user) == _additional->restrictedRights.cend()
						&& _additional->external.find(user) == _additional->external.cend()
						&& _additional->kicked.find(user) == _additional->kicked.cend()
						&& _additional->creator != user) {
						_additional->infoNotLoaded.emplace(user);
					}
					delegate()->peerListSearchAddRow(user);
				}
			}
		}
		delegate()->peerListSearchRefreshRows();
	}
}

void AddParticipantBoxSearchController::addChatsContacts() {
	_chatsContactsAdded = true;

	auto filterWordList = _query.split(cWordSplit(), QString::SkipEmptyParts);
	auto wordsCount = filterWordList.size();
	auto wordList = QStringList();
	wordList.reserve(wordsCount);
	for_const (auto &word, filterWordList) {
		auto trimmed = word.trimmed();
		if (!trimmed.isEmpty()) {
			wordList.push_back(trimmed);
		}
	}
	if (wordList.empty()) {
		return;
	}

	auto getSmallestIndex = [&wordList](Dialogs::IndexedList *list) -> const Dialogs::List* {
		if (list->isEmpty()) {
			return nullptr;
		}

		auto result = (const Dialogs::List*)nullptr;
		for_const (auto &word, wordList) {
			auto found = list->filtered(word[0]);
			if (found->isEmpty()) {
				return nullptr;
			}
			if (!result || result->size() > found->size()) {
				result = found;
			}
		}
		return result;
	};
	auto dialogsIndex = getSmallestIndex(App::main()->dialogsList());
	auto contactsIndex = getSmallestIndex(App::main()->contactsNoDialogsList());

	auto allWordsAreFound = [&wordList](const OrderedSet<QString> &names) {
		auto hasNamePartStartingWith = [&names](const QString &word) {
			for_const (auto &namePart, names) {
				if (namePart.startsWith(word)) {
					return true;
				}
			}
			return false;
		};

		for_const (auto &word, wordList) {
			if (!hasNamePartStartingWith(word)) {
				return false;
			}
		}
		return true;
	};
	auto filterAndAppend = [this, allWordsAreFound](const Dialogs::List *list) {
		if (!list) {
			return;
		}

		for_const (auto row, *list) {
			if (auto user = row->history()->peer->asUser()) {
				if (allWordsAreFound(user->names)) {
					delegate()->peerListSearchAddRow(user);
				}
			}
		}
	};
	filterAndAppend(dialogsIndex);
	filterAndAppend(contactsIndex);
	delegate()->peerListSearchRefreshRows();
}

} // namespace Profile
