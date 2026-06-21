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
                // Ensure we return to the previous screen (menu), not directly to book reading
                FragmentTransaction ft = activity.getSupportFragmentManager().beginTransaction();
                ft.replace(R.id.book_container, new BookReadingFragment());
                ft.commit();
            });
        }
    }

    private void setupMenuItems() {
        // Example implementation; actual logic depends on your menu structure
        // This would typically involve finding buttons and setting listeners
    }
}