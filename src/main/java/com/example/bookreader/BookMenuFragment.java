package com.example.bookreader;

import android.os.Bundle;
import android.view.LayoutInflater;
import android.view.View;
import android.view.ViewGroup;
import android.widget.Button;
import android.widget.ImageView;
import android.widget.TextView;
import androidx.fragment.app.Fragment;
import androidx.fragment.app.FragmentActivity;
import androidx.fragment.app.FragmentTransaction;

public class BookMenuFragment extends Fragment {

    private View rootView;
    private BookManager bookManager;
    private FragmentActivity activity;

    @Override
    public View onCreateView(LayoutInflater inflater, ViewGroup container, Bundle savedInstanceState) {
        rootView = inflater.inflate(R.layout.fragment_book_menu, container, false);
        activity = getActivity();
        bookManager = BookManager.getInstance(activity);

        setupBackButton();
        setupMenuItems();

        return rootView;
    }

    private void setupBackButton() {
        Button backButton = rootView.findViewById(R.id.back_button);
        if (backButton != null) {
            backButton.setOnClickListener(v -> {
                // Ensure we return to the previous screen (menu), not directly to book
                if (activity instanceof BookReaderActivity) {
                    ((BookReaderActivity) activity).showBookMenu();
                }
            });
        }
    }

    private void setupMenuItems() {
        // Example implementation; actual logic depends on menu items
        TextView goToText = rootView.findViewById(R.id.go_to_text);
        TextView selectChapterText = rootView.findViewById(R.id.select_chapter_text);
        TextView showQRText = rootView.findViewById(R.id.show_qr_text);

        goToText.setOnClickListener(v -> {
            // Handle "Go to %" logic
            ((BookReaderActivity) activity).showGoToDialog();
        });

        selectChapterText.setOnClickListener(v -> {
            // Handle "Select Chapter" logic
            ((BookReaderActivity) activity).showChapterSelection();
        });

        showQRText.setOnClickListener(v -> {
            // Handle "Show Page as QR" logic
            ((BookReaderActivity) activity).showQRCodePage();
        });
    }

    @Override
    public void onResume() {
        super.onResume();
        // Ensure menu is visible when fragment is active
        ((BookReaderActivity) activity).setActionBarTitle("Menu");
    }
}