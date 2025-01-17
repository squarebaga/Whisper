#include "stdafx.h"
#include "DryRunCheckbox.h"

static const LPCTSTR regValDryrun= L"dryrun";

void DryRunCheckbox::initialize(HWND owner, int idc, AppState& state)
{
	m_hWnd = GetDlgItem(owner, idc);
	assert(nullptr != m_hWnd);

	if (state.boolLoad(regValDryrun))
		::SendMessage(m_hWnd, BM_SETCHECK, BST_CHECKED, 0L);
}

bool DryRunCheckbox::checked()
{
	assert(nullptr != m_hWnd);
	const int state = (int)::SendMessage(m_hWnd, BM_GETCHECK, 0, 0);
	return state == BST_CHECKED;
}

void DryRunCheckbox::saveSelection(AppState& state)
{
	state.boolStore(regValDryrun, checked());
}