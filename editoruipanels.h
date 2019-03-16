﻿/*
This Source Code Form is subject to the
terms of the Mozilla Public License, v.
2.0. If a copy of the MPL was not
distributed with this file, You can
obtain one at
http://mozilla.org/MPL/2.0/.
*/

#pragma once

#include "uilayer.h"
#include "Classes.h"

class itemproperties_panel : public ui_panel {

public:
    itemproperties_panel( std::string const &Name, bool const Isopen )
        : ui_panel( Name, Isopen )
    {}

    void update( scene::basic_node const *Node );
	void render() override;

private:
// methods
    void update_group();
	bool render_group();

// members
    scene::basic_node const *m_node { nullptr }; // scene node bound to the panel
    scene::group_handle m_grouphandle { null_handle }; // scene group bound to the panel
    std::string m_groupprefix;
    std::vector<text_line> m_grouplines;
};

class nodebank_panel : public ui_panel {
	std::vector<std::shared_ptr<std::string>> m_nodebank;
	std::shared_ptr<std::string> m_selectedtemplate;

public:
	enum edit_mode {
		MODIFY,
		COPY,
		ADD
	};

	edit_mode mode = MODIFY;

	nodebank_panel();

	void render_contents() override;
	void add_template(const std::string &desc);
	const std::string* get_active_template();
};
