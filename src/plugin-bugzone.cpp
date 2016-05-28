/*
 *  Created on: 28 Jan 2013
 *      Author: oaskivvy@gmail.com
 */

/*-----------------------------------------------------------------.
| Copyright (C) 2012 SooKee oaskivvy@gmail.com                     |
'------------------------------------------------------------------'

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
02110-1301, USA.

http://www.gnu.org/licenses/gpl-2.0.html

'-----------------------------------------------------------------*/

#include <regex>
#include <string>
#include <ctime>
#include <array>

#include <skivvy/plugin-bugzone.h>
#include <skivvy/plugin-chanops.h>

#include <sookee/types/basic.h>
#include <sookee/log.h>
#include <sookee/bug.h>

#include <skivvy/stl.h>
//#include <skivvy/logrep.h>
#include <skivvy/irc.h>
#include <skivvy/cal.h>
#include <skivvy/ios.h>
#include <skivvy/utils.h>
#include <skivvy/logrep.h>


//#include <pcrecpp.h>

namespace skivvy { namespace bugzone {

using namespace sookee;
using namespace sookee::types;
using namespace sookee::log;
using namespace sookee::bug;

using namespace skivvy;
using namespace skivvy::irc;
using namespace skivvy::utils;
using namespace skivvy::ircbot;

//using namespace pcrecpp;

// You MUST have this macro and it MUST name your plugin class
IRC_BOT_PLUGIN(BugzoneIrcBotPlugin);
PLUGIN_INFO("bugzone", "Bug Tracker", "0.0");

const str STORE_FILE_KEY = "bugzone.store.file";
const str DEFAULT_STORE_FILE = "bugzone-store.txt";

const auto BUG_VERSION_KEY = store2::make_whole_key("bugzone", "version");

// store keys and key parts
const auto BUG_PREFIX = store2::make_partial_key<3>("bug");


const auto BUG_ENTRY_PREFIX = BUG_PREFIX/"entry"; //store2::make_partial_key<3>("bug", "entry");

const auto BUG_DESC_PREFIX = BUG_PREFIX/"desc";
const auto BUG_PERP_PREFIX = BUG_PREFIX/"perp";
const auto BUG_STAT_PREFIX = BUG_PREFIX/"stat";
const auto BUG_NOTE_PREFIX = BUG_PREFIX/"note";
const auto BUG_DATE_PREFIX = BUG_PREFIX/"date";
const auto BUG_ASGN_PREFIX = BUG_PREFIX/"asgn";
const auto BUG__MOD_PREFIX = BUG_PREFIX/"_mod";
const auto BUG__ETA_PREFIX = BUG_PREFIX/"_eta";
const auto BUG_DEPS_PREFIX = BUG_PREFIX/"deps"; // depends on
const auto BUG__DUP_PREFIX = BUG_PREFIX/"_dup"; // duplicate of

const str BUG_STAT_N = "new";
const str BUG_STAT_A = "assigned";
const str BUG_STAT_W = "wontfix";
const str BUG_STAT_F = "fixed";

const auto BUG_DEV_KEY = store2::make_whole_key("dev");

// bug.desc.<id>: Bad stuff happens
// bug.perp.<id>: sookee|~SooKee@SooKee.users.quakenet.org
// bug.stat.<id>: new|fixed|wontfix|etc...
// bug.note.<id>: Some note
// bug.note.<id>: Another note
// bug.date.<id>: 2013-02-23
// bug.asgn.<id>: sookee
// bug._mod.<id>: rconics
// bug._eta.<id>: +2 days|2013-02-28

BugzoneIrcBotPlugin::BugzoneIrcBotPlugin(IrcBot& bot)
: BasicIrcBotPlugin(bot)
, store(new store2::BackupStore(bot.getf(STORE_FILE_KEY, DEFAULT_STORE_FILE)))
, chanops(bot, "chanops")
{
}

BugzoneIrcBotPlugin::~BugzoneIrcBotPlugin() {}

// Workflow

// new - new bugs
// fix - fixed
// die - won't fix
// dup: #id - duplicate of id

str BugzoneIrcBotPlugin::get_user(const message& msg)
{
	bug_fun();
	bug_var(chanops);
	str userhost = msg.get_userhost();
	// chanops user | msg.userhost

	if(chanops && chanops->api(ChanopsApi::is_userhost_logged_in, {userhost}).empty())
	{
		str_vec r = chanops->api(ChanopsApi::get_userhost_username, {userhost});
		if(!r.empty())
			return r[0];
	}

	return userhost;
}

// bug.desc.<id>: Bad stuff happens
// bug.perp.<id>: sookee|~SooKee@SooKee.users.quakenet.org
// bug.date.<id>: 2013-02-23
// bug.asgn.<id>: sookee
// bug._mod.<id>: rconics
// bug._eta.<id>: +2 days|2013-02-28
// bug.stat.<id>: new|fixed|wontfix|etc...
// bug.note.<id>: Some note
// bug.note.<id>: Another note

void BugzoneIrcBotPlugin::bug_reply(const message& msg, const str& prompt, const str& id)
{
	bot.fc_reply(msg, prompt + "    id: " + id);
	if(store->has(BUG_STAT_PREFIX/id))
		bot.fc_reply(msg, prompt + "status: " + store->get(BUG_STAT_PREFIX/id));
	if(store->has(BUG_DATE_PREFIX/id))
		bot.fc_reply(msg, prompt + "  date: " + store->get(BUG_DATE_PREFIX/id));
	if(store->has(BUG_DESC_PREFIX/id))
		bot.fc_reply(msg, prompt + "  desc: " + store->get(BUG_DESC_PREFIX/id));
	if(store->has(BUG__ETA_PREFIX/id))
		bot.fc_reply(msg, prompt + "   eta: " + store->get(BUG__ETA_PREFIX/id));
	for(const str note: store->get_vec((BUG_NOTE_PREFIX/id)))
		bot.fc_reply(msg, prompt + "  note: " + note);
}

str stamp(time_t now = std::time(0))
{
	return cal::date_t(now).format(cal::date_t::FORMAT_ISO_8601);
}

static const str_map attrs =
{
	{"note", "note"}
	, {"eta", "_eta"}
	, {"status", "stat"}
	, {"assign", "asgn"}
	, {"mod", "_mod"}
	, {"dup", "_dup"}
};

static const str_set stats = {"new","open","fixed","wontfix","deleted"};

// negate opper
// (<=|>=|<|>|=)
const str_map nopers = {{"<=",">"},{">=","<"},{"<",">="},{">","<="},{"=","="}};

template<typename Container>
str join(const Container& c, const str& sep = ", ")
{
	str s, ret;
	for(const str& v: c)
		{ ret += s + v; s = sep; }
	return ret;
}

str to_id(int n)
{
	str s = std::to_string(n);
	if(s.size() < 6)
		s = str(6 - s.size(), '0') + s;
	return s;
}

bool BugzoneIrcBotPlugin::do_bug(const message& msg)
{
	BUG_COMMAND(msg);

	if(msg.get_user_params().empty())
		return false;

	static const str prompt = IRC_BOLD + IRC_COLOR + IRC_Purple + "bug"
		+ IRC_COLOR + IRC_Black + ": " + IRC_NORMAL;

	// !bug <text> - enter a new bug with desc(ription) <text>
	// !bug #n - display a curent bug #n by number.
	// !bug #n +(note|eta|status|assign|mod|dup) <text> - add notes, change status, eta etc...

	if(msg.get_user_params()[0] == '#') // print or edit current bug
	{
		siz n = 0;
		siss iss(msg.get_user_params().substr(1));
		if(!(iss >> n))
			return bot.cmd_error(msg, prompt + "Expected bug tracking id after # (eg. #2365");

		str id = to_id(n);

		str line, attr, text;
		while(sgl(iss, line, '+'))
		{
			if(rtrim(line).empty())
				continue;

			if(!ios::getstring(siss(line) >> attr >> std::ws, text))
			{
				bot.fc_reply(msg, REPLY_PROMPT + "Expected: +(note|eta|status|assign|mod|dup) <text>");
				continue;
			}

			trim(attr);
			trim(text);
			bug_var(attr);
			bug_var(text);

			if(attr == "note") // additive
			{
				store->add(BUG_NOTE_PREFIX/id, stamp() + ": " + text);
			}
			else if(attrs.find(attr) != attrs.end())
			{
				attr = attrs.at(attr);

				if(attr == "stat" && stats.find(text) == stats.end())
				{
					bot.fc_reply(msg, REPLY_PROMPT + "Unknown status: " + text + ", use: " + join(stats));
					continue;
				}
				else if(attr == "asgn")
				{
					if(!stl::found(store->get_vec(BUG_DEV_KEY), text))
					{
						bot.fc_reply(msg, REPLY_PROMPT + "Unknown developer: " + text);
						continue;
					}
				}
				else if(attr == "_dup")
				{
					siz n;
					if(text.empty() || text[0] != '#' || !(siss(text).ignore() >> n))
					{
						bot.fc_reply(msg, REPLY_PROMPT + "Expected bug number #<n>");
						continue;
					}
				}
//				store->set("bug." + attr + '.' + id, stamp() + ": " + text);
				store->set(BUG_PREFIX/attr/id, stamp() + ": " + text);
				bot.fc_reply(msg, REPLY_PROMPT + "done.");
			}
			else
			{
				log("ERROR: Unknown attribute: " << attr);
				bot.fc_reply(msg, REPLY_PROMPT + "Unknown attribute: " + attr
					+  "Expected: +(note|eta|status|assign|mod|dup) <text>");
				continue;
			}
		}

		lock_guard lock(mtx);
		bug_reply(msg, REPLY_PROMPT, id);

		return true;
	}

	// add new bug
	str user = get_user(msg);
	bug_var(user);

	// bug.desc.<id>: Bad stuff happens
	// bug.perp.<id>: sookee|~SooKee@SooKee.users.quakenet.org
	// bug.date.<id>: 2013-02-23
	// bug.asgn.<id>: sookee
	// bug._mod.<id>: rconics
	// bug._eta.<id>: +2 days|2013-02-28
	// bug.stat.<id>: new|fixed|wontfix|etc...
	// bug.note.<id>: Some note
	// bug.note.<id>: Another note

	lock_guard lock(mtx);
	str id = to_id(store->get(BUG_PREFIX/"last"/"id", 0) + 1);

	store->set(BUG_PREFIX/"last"/"id", id);

	store->add(BUG_DESC_PREFIX/id, msg.get_user_params());
	store->add(BUG_PERP_PREFIX/id, user);
	store->add(BUG_DATE_PREFIX/id, cal::date_t(std::time(0)).format(cal::date_t::FORMAT_ISO_8601));
	store->add(BUG_STAT_PREFIX/id, BUG_STAT_N);

	bot.fc_reply(msg, REPLY_PROMPT + "Your bug has been filed with tracking number #" +id);
	return true;
}

//#define bug_cnt(c) do{bug(#c << ':');for(auto v: c) bug('\t' << v);}while(false)

template<typename T>
void assign_type(const std::string& s, T& t)
{
	std::istringstream(s) >> t;
}

void assign_type(const std::string& s, std::string& t)
{
	t = s;
}

void set_arg(const std::smatch&, int)
{
}

template<typename Arg>
void set_arg(const std::smatch& m, int i, Arg& arg)
{
	assign_type(m.str(i), arg);
}

template<typename Arg, typename... Args>
void set_arg(const std::smatch& m, int i, Arg& arg, Args&... args)
{
	assign_type(m.str(i), arg);
	set_arg(m, i + 1, args...);
}

template<typename... Args>
bool full_match(const std::string& s, const std::regex& e, Args&... args)
{
	std::smatch m;
	if(!std::regex_match(s,  m,  e))
		return false;

	set_arg(m, 1, args...);

	return true;
}

template<size_t SIZE> // full size of whole key
class store_key
{
	str type; // type.k1.k2.kn: record
	std::vector<str> keys;

	template<size_t ASIZE>
	store_key(str type, std::array<str, ASIZE> arr,
		typename std::enable_if<ASIZE <= SIZE, bool>::type = false)
	: type(type), keys(arr.begin(), arr.end())
	{
//		bug_fun();
	}

public:
	template<typename... Args>
	store_key(str type, Args&&... args)
	: store_key(type, std::array<str, sizeof...(Args)>({std::forward<Args>(args)...}))
	{
//		bug_fun();
	}

	constexpr uns size() const { return SIZE; }

	operator std::string() const
	{
		auto sep = "";
		std::string s;
		for(auto const& key: keys)
		{
			s += sep + key;
			sep = ".";
		}
		return s + partial() ? ".*":"";
	}

	bool full() const { return keys.size() == SIZE; }
	bool partial() const { return !full(); }

	void dump() const
	{
		bug_fun();
		bug_var(type);
		bug_cnt(keys);
	}
};

bool BugzoneIrcBotPlugin::do_buglist(const message& msg)
{
	BUG_COMMAND(msg);
	store_key<3> delete_me("bug");
	// !buglist *(new|fixed|dead|dups) - list bugs if associated with caller

	// !blist +stat = new, dead +stat = fixed +date <= (now|date)

//	static const str prompt = IRC_BOLD + IRC_COLOR + IRC_Green + "blist"
//		+ IRC_COLOR + IRC_Black + ": " + IRC_NORMAL;

	// TODO: finish this
//	return bot.cmd_error(msg, "Feature incomplete.");

	str_set stats;
	str_set dates_eq;
	str_set dates_lt;
	str_set dates_le;
	str_set dates_gt;
	str_set dates_ge;

	str attr, line;
	siss iss(msg.get_user_params());

	while(sgl(iss, line, '+'))
	{
		if(rtrim(line).empty())
			continue;

		bug_var(line);

		if(!line.find("status"))
		{
			if(!sgl(sgl(siss(line), attr, '='), line))
			{
				log("ERROR: Expected: status = (new|fixed|dup|...)");
				continue;
			}

			trim(attr);
			trim(line);

			if(attrs.count(attr))
				attr = attrs.at(attr); // normalize

			bug_var(attr);
			bug_var(line);

			str stat;
			siss iss(line);
			if(attr == "stat")
				while(sgl(iss, stat, ','))
					if(!trim(stat).empty())
						stats.insert(stat);
		}
		else if(!line.find("date"))
		{
			// date <= <date>
			str oper, date;

			{
				std::smatch m;
				std::regex e("([^<=>]+)(<=|>=|<|>|=)([^<=>]+)");
	//			if(!RE("([^<=>]+)(<=|>=|<|>|=)([^<=>]+)").FullMatch(line, &attr, &oper, &date))
				if(!std::regex_match(line, m, e))
				{
					bot.fc_reply(msg, REPLY_PROMPT + "Expected: +date (<=|>=|<|>|=) 31/12/9999");
					continue;
				}

				attr = m.str(1);
				oper = m.str(2);
				date = m.str(3);
			}

			trim(attr);
			trim(date);

			if(attrs.count(attr))
				attr = attrs.at(attr); // normalize

			bug_var(attr);
			bug_var(oper);
			bug_var(date);

			if(attr != "date")
			{
				log("ERROR: Unknown attribute: " << attr);
				continue;
			}

			// parse date format

			siz d, m, y;

			if(date == "today")
			{
				cal::date_t d(time(0));
				date = d.format(cal::date_t::FORMAT_ISO_8601);

				if(nopers.count(oper))
					oper = nopers.at(oper);
			}
			else if(date == "yesterday")
			{
				cal::date_t cdate(time(0));
				cdate.dec_day();
				date = cdate.format(cal::date_t::FORMAT_ISO_8601);

				if(nopers.count(oper))
					oper = nopers.at(oper);
			}
//			else if(RE("(\\d+)\\s+days?").FullMatch(date, &d))
			else if(full_match(date, std::regex("(\\d+)\\s+days?"), d))
			{
				if(d > 6)
				{
					log("ERROR: More than 6 days... use weeks");
					continue;
				}
				cal::date_t cdate(time(0));
				while(--d)
					cdate.dec_day();
				date = cdate.format(cal::date_t::FORMAT_ISO_8601);

				if(nopers.count(oper))
					oper = nopers.at(oper);
			}
//			else if(RE("(\\d+)\\s+weeks?").FullMatch(date, &d))
			else if(full_match(date, std::regex("(\\d+)\\s+weeks?"), d))
			{
				if(d > 4)
				{
					log("ERROR: More than 4 weeks... use months");
					continue;
				}
				cal::date_t cdate(time(0));
				d *= 7;
				while(--d)
					cdate.dec_day();
				date = cdate.format(cal::date_t::FORMAT_ISO_8601);

				if(nopers.count(oper))
					oper = nopers.at(oper);
			}
//			else if(RE("(\\d+)\\s+months?").FullMatch(date, &d))
			else if(full_match(date, std::regex("(\\d+)\\s+months?"), d))
			{
				if(d > 12)
				{
					log("ERROR: More than 12 months... use years");
					continue;
				}
				cal::date_t cdate(time(0));
				while(--d)
					cdate.dec_month();
				date = cdate.format(cal::date_t::FORMAT_ISO_8601);

				if(nopers.count(oper))
					oper = nopers.at(oper);
			}
//			else if(RE("(\\d+)\\s+years?").FullMatch(date, &d))
			else if(full_match(date, std::regex("(\\d+)\\s+years?"), d))
			{
				cal::date_t cdate(time(0));
				while(--d)
					cdate.dec_year();
				date = cdate.format(cal::date_t::FORMAT_ISO_8601);

				if(nopers.count(oper))
					oper = nopers.at(oper);
			}
//			else if(RE("\\d{4}-\\d{2}-\\d{2}").FullMatch(date))
			else if(full_match(date, std::regex("\\d{4}-\\d{2}-\\d{2}")))
			{
				bug("PERFECT FORMAT: " << date);
			}
//			else if(RE("(\\d{1,2})[-/](\\d{1,2})[-/](\\d{2,4})").FullMatch(date, &d, &m, &y))
			else if(full_match(date, std::regex("(\\d{1,2})[-/](\\d{1,2})[-/](\\d{2,4})"), d, m, y))
			{
				soss oss;
				y = y < 100 ? 2000 + y : y;
				oss << y << "-" << (m < 10 ? "0":"") << m << "-" << (d < 10 ? "0":"") << d;
				date = oss.str();
			}
			else
			{
				log("ERROR: Expected: date format yyyy-mm-dd or d(d)/m(m)/yy(yy) or d(d)-m(m)-yy(yy)");
				continue;
			}

			bug_var(d);
			bug_var(m);
			bug_var(y);

			bug_var(date);

			if(oper == "=")
				dates_eq.insert(date);
			else if(oper == "<")
				dates_lt.insert(date);
			else if(oper == "<=")
				dates_le.insert(date);
			else if(oper == ">")
				dates_gt.insert(date);
			else if(oper == ">=")
				dates_ge.insert(date);
			else
			{
				log("ERROR: Unknown operand: " << oper);
				continue;
			}
		}
		else
		{
			log("ERROR: Unknown attribute: " << line);
			continue;
		}
	}

	if(stats.empty())
		stats.insert("*");

	bug_cnt(stats);
	bug_cnt(dates_eq);
	bug_cnt(dates_lt);
	bug_cnt(dates_le);
	bug_cnt(dates_gt);
	bug_cnt(dates_ge);

	str id;
	str_set ids;

	bug("Adding ids based in stat:");
//	for(const str& key: store->get_keys_if_wild(BUG_STAT_PREFIX + "*"))
	for(const str& key: store->get_keys_that_match(BUG_STAT_PREFIX))
		for(const str& stat: stats)
			if(stat == "*" || stat == store->get(key))
				if(full_match(key, std::regex("bug\\.[^.]+\\.([^:]+)"), id))
//				if(RE("bug\\.[^.]+\\.([^:]+)").FullMatch(key, &id))
					{ ids.insert(id); break; }

	bug("Erasing keys based in date:");
	str store_date;
	for(str_set::iterator idi = ids.begin(); idi != ids.end();)
	{
		store_date = store->get(BUG_DATE_PREFIX/(*idi));
		bug_var(store_date);

		for(const str& date: dates_eq)
			if(store_date != date)
				{ idi = ids.erase(idi); goto next; }

		for(const str& date: dates_lt)
			if(store_date >= date)
				{ idi = ids.erase(idi); goto next; }

		for(const str& date: dates_le)
			if(store_date > date)
				{ idi = ids.erase(idi); goto next; }

		for(const str& date: dates_gt)
			if(store_date <= date)
				{ idi = ids.erase(idi); goto next; }

		for(const str& date: dates_ge)
			if(store_date < date)
				{ idi = ids.erase(idi); goto next; }

		++idi;
		next:;
	}

	bug_cnt(ids);

	lock_guard lock(mtx);

	for(const str& id: ids)
		bug_reply(msg, REPLY_PROMPT, id);

	return true;
}

struct dev_t
{
	str handle;
	str chanops;

	friend soss& operator<<(soss& oss, const dev_t dev)
	{
		oss << '{' << dev.handle << ' ' << dev.chanops << '}';
		return oss;
	}

	friend siss& operator>>(siss& iss, dev_t dev)
	{
		str o;
		if(!getobject(iss, o) || !(siss(o) >> dev.handle >> dev.chanops))
			log("ERROR: bugzone: protocol error");
		return iss;
	}
};

bool BugzoneIrcBotPlugin::do_dev(const message& msg)
{
	BUG_COMMAND(msg);

	// !dev (add|mod|info|) <text>

	// !dev add <dev-handle> <email> "name" ?({chanops: <chanops-user>})
	// !dev <dev-handle> +chanops <chanops-user> // link with chanops, tag dev
	// !dev <dev-handle> -chanops <chanops-user> // remove chanops tag from dev

	// dev.<dev-handle>:

	return true;
}

bool BugzoneIrcBotPlugin::do_feature(const message& msg)
{
	BUG_COMMAND(msg);
//	if(msg.get_user_params().empty())
//		return false;
//
//	lock_guard lock(mtx);
//	siz id = store->get("feature.id", 0);
//	store->set("feature.id", ++id);
//	store->add("feature." + std::to_string(id) + ".new", " \"" + msg.get_user_params() + "\"");
//	bot.fc_reply(msg, "Your feature request has been filed with tracking number " + std::to_string(id));
	return true;
}

// INTERFACE: BasicIrcBotPlugin

bool BugzoneIrcBotPlugin::initialize()
{
	// Commands MUST start with ! (unless they are sent by PM)
	if(!store->has(BUG_VERSION_KEY) || store->get(BUG_VERSION_KEY) == "0.0")
	{
		// upgrade
		log("bugzone: Upgrading store from v0.0 to v0.1");

		// bug.entry.<id>.<state>: <user> "<message"

		siz n;
		str id, skip, stat, text, user;
		char c;
//		for(const str& key: store->get_keys_if_wild(BUG_ENTRY_PREFIX + "*"))
		for(const str& key: store->get_keys_that_match(BUG_ENTRY_PREFIX))
		{
			if(key.size() <= BUG_ENTRY_PREFIX.size())
				continue;
			if(!(siss(key.substr(BUG_ENTRY_PREFIX.size())) >> n >> c >> stat))
				continue;
			if(!sgl(sgl(siss(store->get(key)) >> user, skip, '"'), text, '"'))
				continue;

			id = to_id(n);
			store->add(BUG_DESC_PREFIX/id, text);
			store->add(BUG_PERP_PREFIX/id, user);
			store->add(BUG_STAT_PREFIX/id, stat);
			store->add(BUG_NOTE_PREFIX/id, "Record upgraded from v0.0 to v0.1 (see date for when)");
			store->add(BUG_DATE_PREFIX/id, std::to_string(std::time(0)));
			store->clear(key);
		}

		// bug.desc.<id>: Bad stuff happens
		// bug.perp.<id>: sookee|~SooKee@SooKee.users.quakenet.org
		// bug.date.<id>: 2013-02-23
		// bug.asgn.<id>: sookee
		// bug._mod.<id>: rconics
		// bug._eta.<id>: +2 days|2013-02-28
		// bug.stat.<id>: new|fixed|wontfix|etc...
		// bug.note.<id>: Some note
		// bug.note.<id>: Another note

		store->set(BUG_VERSION_KEY, "0.1");
	}
	add
	({
		// !bug <text> - enter a new bug with description <text>
		// !bug #n - display a curent bug #n by number.
		// !bug #n +(note|eta|status|assign|mod|dup) <text> - add notes, change status, eta etc...
		"!bug"
		, "<text>                                         Enter a new bug with description <text>."
		  "\n #n                                          Display bug #n by number."
		  "\n #n +(note|eta|status|assign|mod|dup) <text> Add note, eta etc..."
		, [&](const message& msg){ do_bug(msg); }
	});
	add
	({
		"!buglist"
		, "!buglist *(new|dead|fixed|dups) list your bugs, optionally by category."
		, [&](const message& msg){ do_buglist(msg); }
	});
//	add
//	({
//		"!feature"
//		, "!feature <description> Request a feature|modification."
//		, [&](const message& msg){ do_feature(msg); }
//	});

	add
	({
		"!dev"
		, "!dev (add|mod|info|) <text>"
		, [&](const message& msg){ do_dev(msg); }
	});

	//bot.add_monitor(*this);

	return true;
}

// INTERFACE: IrcBotPlugin

str BugzoneIrcBotPlugin::get_id() const { return ID; }
str BugzoneIrcBotPlugin::get_name() const { return NAME; }
str BugzoneIrcBotPlugin::get_version() const { return VERSION; }

void BugzoneIrcBotPlugin::exit()
{
	// clean up (stop threads etc...)
}

}} // skivvy::bugzon
